/**********************************************************************

  stackprof.c - Sampling call-stack frame profiler for MRI.

  vim: noexpandtab shiftwidth=4 tabstop=8 softtabstop=4

**********************************************************************/

#include <ruby/ruby.h>
#include <ruby/debug.h>
#include <ruby/st.h>
#include <ruby/io.h>
#include <ruby/intern.h>
#include <ruby/version.h>
#include <signal.h>
#include <sys/time.h>
#include <pthread.h>
#include "vendor/uthash.h"

#define BUF_SIZE 2048

typedef struct {
    size_t total_samples;
    size_t caller_samples;
    st_table *edges;
    st_table *lines;
} frame_data_t;

typedef struct {
    VALUE obj;
    int num;
    VALUE *frames;
    int *lines_buffer;
    int living;
    VALUE flags;
    VALUE klass;
    size_t memsize;
    UT_hash_handle hh;
} allocation_info_t;

static struct {
    int running;
    int raw;
    int aggregate;

    VALUE mode;
    VALUE interval;
    VALUE out;

    VALUE *raw_samples;
    size_t raw_samples_len;
    size_t raw_samples_capa;
    size_t raw_sample_index;

    size_t overall_signals;
    size_t overall_samples;
    size_t during_gc;
    st_table *frames;

    allocation_info_t *frames_heap_live;
    int heap_all;
    VALUE frames_buffer[BUF_SIZE];
    int lines_buffer[BUF_SIZE];
} _stackprof;

static VALUE sym_object, sym_wall, sym_cpu, sym_custom, sym_name, sym_file, sym_line;
static VALUE sym_samples, sym_total_samples, sym_missed_samples, sym_edges, sym_lines;
static VALUE sym_version, sym_mode, sym_interval, sym_raw, sym_frames, sym_out, sym_aggregate;
static VALUE sym_gc_samples, objtracer, sym_heap, objtracer_newobj, objtracer_freeobj, sym_heap_all;
static VALUE gc_hook;
static VALUE rb_mStackProf;
static size_t rvalue_size;

static void stackprof_newobj_handler(VALUE, void*);
static void stackprof_newobj_handler_heap(VALUE, void*);
static void stackprof_freeobj_handler_heap(VALUE, void*);
static void stackprof_signal_handler(int sig, siginfo_t* sinfo, void* ucontext);
static void stackprof_process_sample(VALUE *frames_buffer, int *lines_buffer, int num);

static VALUE
stackprof_start(int argc, VALUE *argv, VALUE self)
{
    struct sigaction sa;
    struct itimerval timer;
    VALUE opts = Qnil, mode = Qnil, interval = Qnil, out = Qfalse;
    int raw = 0, aggregate = 1, heap_all = 0;

    if (_stackprof.running)
	return Qfalse;

    rb_scan_args(argc, argv, "0:", &opts);

    if (RTEST(opts)) {
	mode = rb_hash_aref(opts, sym_mode);
	interval = rb_hash_aref(opts, sym_interval);
	out = rb_hash_aref(opts, sym_out);

	if (RTEST(rb_hash_aref(opts, sym_raw)))
	    raw = 1;
	if (rb_hash_lookup2(opts, sym_aggregate, Qundef) == Qfalse)
	    aggregate = 0;
        if (RTEST(rb_hash_aref(opts, sym_heap_all)))
	    heap_all = 1;
    }
    if (!RTEST(mode)) mode = sym_wall;

    if (!_stackprof.frames) {
	_stackprof.frames = st_init_numtable();
	_stackprof.overall_signals = 0;
	_stackprof.overall_samples = 0;
	_stackprof.during_gc = 0;
    }

    if (!_stackprof.frames_heap_live) {
        _stackprof.frames_heap_live = NULL;
    }

    if (mode == sym_object) {
	if (!RTEST(interval)) interval = INT2FIX(1);

	objtracer = rb_tracepoint_new(Qnil, RUBY_INTERNAL_EVENT_NEWOBJ, stackprof_newobj_handler, 0);
	rb_tracepoint_enable(objtracer);
    } else if (mode == sym_wall || mode == sym_cpu) {
	if (!RTEST(interval)) interval = INT2FIX(1000);

	sa.sa_sigaction = stackprof_signal_handler;
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(mode == sym_wall ? SIGALRM : SIGPROF, &sa, NULL);

	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = NUM2LONG(interval);
	timer.it_value = timer.it_interval;
	setitimer(mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
    } else if (mode == sym_custom) {
	/* sampled manually */
	interval = Qnil;
    } else if (mode == sym_heap) {
        objtracer_newobj = rb_tracepoint_new(Qnil, RUBY_INTERNAL_EVENT_NEWOBJ, stackprof_newobj_handler_heap, 0);
        rb_tracepoint_enable(objtracer_newobj);

        objtracer_freeobj = rb_tracepoint_new(Qnil, RUBY_INTERNAL_EVENT_FREEOBJ, stackprof_freeobj_handler_heap, 0);
        rb_tracepoint_enable(objtracer_freeobj);
    } else {
	rb_raise(rb_eArgError, "unknown profiler mode");
    }

    _stackprof.running = 1;
    _stackprof.raw = raw;
    _stackprof.aggregate = aggregate;
    _stackprof.mode = mode;
    _stackprof.interval = interval;
    _stackprof.out = out;
    _stackprof.heap_all = heap_all;

    return Qtrue;
}

static size_t
get_object_size(VALUE obj)
{
    VALUE flags = RBASIC(obj)->flags;
    VALUE klass = RBASIC_CLASS(obj);
    size_t objsize = 0;

    if (flags) {
        switch (klass) {
        case T_NONE:
        case T_ICLASS:
        case T_NODE:
        case T_ZOMBIE:
            break;

        case T_CLASS:
            if (FL_TEST(obj, FL_SINGLETON))
                break;
        default:
            if (klass == 0 || rb_obj_is_kind_of(obj, klass)) {
                objsize = rb_obj_memsize_of(obj);
            }
        }
    }

    return objsize + rvalue_size;
}

static VALUE
stackprof_stop(VALUE self)
{
    struct sigaction sa;
    struct itimerval timer;
    allocation_info_t *info, *tmp;

    if (!_stackprof.running)
	return Qfalse;
    _stackprof.running = 0;

    if (_stackprof.mode == sym_object) {
	rb_tracepoint_disable(objtracer);
    } else if (_stackprof.mode == sym_wall || _stackprof.mode == sym_cpu) {
	memset(&timer, 0, sizeof(timer));
	setitimer(_stackprof.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(_stackprof.mode == sym_wall ? SIGALRM : SIGPROF, &sa, NULL);
    } else if (_stackprof.mode == sym_custom) {
	/* sampled manually */
    } else if (_stackprof.mode == sym_heap) {
        // Force GC to cleanup unreferenced live objects
        rb_gc_start();

        rb_tracepoint_disable(objtracer_newobj);
        rb_tracepoint_disable(objtracer_freeobj);

        HASH_ITER(hh, _stackprof.frames_heap_live, info, tmp) {
            if (info->frames) {
                if (info->living && !info->memsize) {
                    info->memsize = 0; //get_object_size(info->obj);
                }
                stackprof_process_sample(info->frames, info->lines_buffer, info->num);
            }
            HASH_DEL(_stackprof.frames_heap_live, info);
            free(info->frames);
            free(info->lines_buffer);
            free(info);
        }
    } else {
	rb_raise(rb_eArgError, "unknown profiler mode");
    }

    return Qtrue;
}

static int
frame_edges_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE edges = (VALUE)arg;

    intptr_t weight = (intptr_t)val;
    rb_hash_aset(edges, rb_obj_id((VALUE)key), INT2FIX(weight));
    return ST_CONTINUE;
}

static int
frame_lines_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE lines = (VALUE)arg;

    size_t weight = (size_t)val;
    size_t total = weight & (~(size_t)0 << (8*SIZEOF_SIZE_T/2));
    weight -= total;
    total = total >> (8*SIZEOF_SIZE_T/2);
    rb_hash_aset(lines, INT2FIX(key), rb_ary_new3(2, ULONG2NUM(total), ULONG2NUM(weight)));
    return ST_CONTINUE;
}

static int
frame_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE frame = (VALUE)key;
    frame_data_t *frame_data = (frame_data_t *)val;
    VALUE results = (VALUE)arg;
    VALUE details = rb_hash_new();
    VALUE name, file, edges, lines;
    VALUE line;

    rb_hash_aset(results, rb_obj_id(frame), details);

    name = rb_profile_frame_full_label(frame);
    rb_hash_aset(details, sym_name, name);

    file = rb_profile_frame_absolute_path(frame);
    if (NIL_P(file))
	file = rb_profile_frame_path(frame);
    rb_hash_aset(details, sym_file, file);

    if ((line = rb_profile_frame_first_lineno(frame)) != INT2FIX(0))
	rb_hash_aset(details, sym_line, line);

    rb_hash_aset(details, sym_total_samples, SIZET2NUM(frame_data->total_samples));
    rb_hash_aset(details, sym_samples, SIZET2NUM(frame_data->caller_samples));

    if (frame_data->edges) {
        edges = rb_hash_new();
        rb_hash_aset(details, sym_edges, edges);
        st_foreach(frame_data->edges, frame_edges_i, (st_data_t)edges);
        st_free_table(frame_data->edges);
        frame_data->edges = NULL;
    }

    if (frame_data->lines) {
	lines = rb_hash_new();
	rb_hash_aset(details, sym_lines, lines);
	st_foreach(frame_data->lines, frame_lines_i, (st_data_t)lines);
	st_free_table(frame_data->lines);
	frame_data->lines = NULL;
    }

    xfree(frame_data);
    return ST_DELETE;
}

static VALUE
stackprof_results(int argc, VALUE *argv, VALUE self)
{
    VALUE results, frames;

    if (!_stackprof.frames || _stackprof.running)
	return Qnil;

    results = rb_hash_new();
    rb_hash_aset(results, sym_version, DBL2NUM(1.1));
    rb_hash_aset(results, sym_mode, _stackprof.mode);
    rb_hash_aset(results, sym_interval, _stackprof.interval);
    rb_hash_aset(results, sym_samples, SIZET2NUM(_stackprof.overall_samples));
    rb_hash_aset(results, sym_gc_samples, SIZET2NUM(_stackprof.during_gc));
    rb_hash_aset(results, sym_missed_samples, SIZET2NUM(_stackprof.overall_signals - _stackprof.overall_samples));

    frames = rb_hash_new();
    rb_hash_aset(results, sym_frames, frames);
    st_foreach(_stackprof.frames, frame_i, (st_data_t)frames);

    st_free_table(_stackprof.frames);
    _stackprof.frames = NULL;

    if (_stackprof.raw && _stackprof.raw_samples_len) {
	size_t len, n, o;
	VALUE raw_samples = rb_ary_new_capa(_stackprof.raw_samples_len);

	for (n = 0; n < _stackprof.raw_samples_len; n++) {
	    len = (size_t)_stackprof.raw_samples[n];
	    rb_ary_push(raw_samples, SIZET2NUM(len));

	    for (o = 0, n++; o < len; n++, o++)
		rb_ary_push(raw_samples, rb_obj_id(_stackprof.raw_samples[n]));
	    rb_ary_push(raw_samples, SIZET2NUM((size_t)_stackprof.raw_samples[n]));
	}

	free(_stackprof.raw_samples);
	_stackprof.raw_samples = NULL;
	_stackprof.raw_samples_len = 0;
	_stackprof.raw_samples_capa = 0;
	_stackprof.raw_sample_index = 0;
	_stackprof.raw = 0;

	rb_hash_aset(results, sym_raw, raw_samples);
    }

    if (argc == 1)
	_stackprof.out = argv[0];

    if (RTEST(_stackprof.out)) {
	VALUE file;
	if (RB_TYPE_P(_stackprof.out, T_STRING)) {
	    file = rb_file_open_str(_stackprof.out, "w");
	} else {
	    file = rb_io_check_io(_stackprof.out);
	}
	rb_marshal_dump(results, file);
	rb_io_flush(file);
	_stackprof.out = Qnil;
	return file;
    } else {
	return results;
    }
}

static VALUE
stackprof_run(int argc, VALUE *argv, VALUE self)
{
    rb_need_block();
    stackprof_start(argc, argv, self);
    rb_ensure(rb_yield, Qundef, stackprof_stop, self);
    return stackprof_results(0, 0, self);
}

static VALUE
stackprof_running_p(VALUE self)
{
    return _stackprof.running ? Qtrue : Qfalse;
}

static inline frame_data_t *
sample_for(VALUE frame)
{
    st_data_t key = (st_data_t)frame, val = 0;
    frame_data_t *frame_data;

    if (st_lookup(_stackprof.frames, key, &val)) {
        frame_data = (frame_data_t *)val;
    } else {
        frame_data = ALLOC_N(frame_data_t, 1);
        MEMZERO(frame_data, frame_data_t, 1);
        val = (st_data_t)frame_data;
        st_insert(_stackprof.frames, key, val);
    }

    return frame_data;
}

static int
numtable_increment_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing)
{
    size_t *weight = (size_t *)value;
    size_t increment = (size_t)arg;

    if (existing)
	(*weight) += increment;
    else
	*weight = increment;

    return ST_CONTINUE;
}

void
st_numtable_increment(st_table *table, st_data_t key, size_t increment)
{
    st_update(table, key, numtable_increment_callback, (st_data_t)increment);
}

void
stackprof_record_sample()
{
    int num;

    _stackprof.overall_samples++;
    num = rb_profile_frames(0, sizeof(_stackprof.frames_buffer) / sizeof(VALUE), _stackprof.frames_buffer, _stackprof.lines_buffer);

    if (_stackprof.mode == sym_heap)
        return;

    stackprof_process_sample(_stackprof.frames_buffer, _stackprof.lines_buffer, num);
}

void
stackprof_process_sample(VALUE *frames_buffer, int *lines_buffer, int num)
{
    int i, n;
    VALUE prev_frame = Qnil;

    if (_stackprof.raw) {
	int found = 0;

	if (!_stackprof.raw_samples) {
	    _stackprof.raw_samples_capa = num * 100;
	    _stackprof.raw_samples = malloc(sizeof(VALUE) * _stackprof.raw_samples_capa);
	}

	if (_stackprof.raw_samples_capa <= _stackprof.raw_samples_len + num) {
	    _stackprof.raw_samples_capa *= 2;
	    _stackprof.raw_samples = realloc(_stackprof.raw_samples, sizeof(VALUE) * _stackprof.raw_samples_capa);
	}

	if (_stackprof.raw_samples_len > 0 && _stackprof.raw_samples[_stackprof.raw_sample_index] == (VALUE)num) {
	    for (i = num-1, n = 0; i >= 0; i--, n++) {
		VALUE frame = frames_buffer[i];
		if (_stackprof.raw_samples[_stackprof.raw_sample_index + 1 + n] != frame)
		    break;
	    }
	    if (i == -1) {
		_stackprof.raw_samples[_stackprof.raw_samples_len-1] += 1;
		found = 1;
	    }
	}

	if (!found) {
	    _stackprof.raw_sample_index = _stackprof.raw_samples_len;
	    _stackprof.raw_samples[_stackprof.raw_samples_len++] = (VALUE)num;
	    for (i = num-1; i >= 0; i--) {
		VALUE frame = frames_buffer[i];
		_stackprof.raw_samples[_stackprof.raw_samples_len++] = frame;
	    }
	    _stackprof.raw_samples[_stackprof.raw_samples_len++] = (VALUE)1;
	}
    }

    for (i = 0; i < num; i++) {
	int line = lines_buffer[i];
	VALUE frame = frames_buffer[i];
	frame_data_t *frame_data = sample_for(frame);

	frame_data->total_samples++;

	if (i == 0) {
	    frame_data->caller_samples++;
	} else if (_stackprof.aggregate) {
	    if (!frame_data->edges)
		frame_data->edges = st_init_numtable();
	    st_numtable_increment(frame_data->edges, (st_data_t)prev_frame, 1);
	}

	if (_stackprof.aggregate && line > 0) {
	    if (!frame_data->lines)
		frame_data->lines = st_init_numtable();
	    size_t half = (size_t)1<<(8*SIZEOF_SIZE_T/2);
	    size_t increment = i == 0 ? half + 1 : half;
	    st_numtable_increment(frame_data->lines, (st_data_t)line, increment);
	}

	prev_frame = frame;
    }
}

static void
stackprof_job_handler(void *data)
{
    static int in_signal_handler = 0;
    if (in_signal_handler) return;
    if (!_stackprof.running) return;

    in_signal_handler++;
    stackprof_record_sample();
    in_signal_handler--;
}

static void
stackprof_signal_handler(int sig, siginfo_t *sinfo, void *ucontext)
{
    _stackprof.overall_signals++;
    if (rb_during_gc())
	_stackprof.during_gc++, _stackprof.overall_samples++;
    else
	rb_postponed_job_register_one(0, stackprof_job_handler, 0);
}

static void
stackprof_newobj_handler(VALUE tpval, void *data)
{
    _stackprof.overall_signals++;
    if (RTEST(_stackprof.interval) && _stackprof.overall_signals % NUM2LONG(_stackprof.interval))
	return;
    stackprof_job_handler(0);
}


static void
stackprof_newobj_handler_heap(VALUE tpval, void *data)
{
    allocation_info_t *info = NULL, *tmp = NULL;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE obj = rb_tracearg_object(tparg);
    int num, replace = 0;

    _stackprof.overall_signals++;

    if (RTEST(_stackprof.interval) && _stackprof.overall_signals % NUM2LONG(_stackprof.interval))
	return;

    _stackprof.overall_samples++;

    HASH_FIND(hh, _stackprof.frames_heap_live, &obj, sizeof(VALUE), info);
    if (!info) {
        info = (allocation_info_t *)malloc(sizeof(allocation_info_t));
    }
    else {
        free(info->frames);
        free(info->lines_buffer);
        replace = 1;
    }

    num = rb_profile_frames(0, sizeof(_stackprof.frames_buffer) / sizeof(VALUE), _stackprof.frames_buffer, _stackprof.lines_buffer);

    info->obj = obj;

    info->num = num;
    info->living = 1;
    info->flags = RBASIC(obj)->flags;
    info->klass = RBASIC_CLASS(obj);

    // We will not compute memory used here, we will do it later. It appears
    // this trace callback is called before the object in question is
    // populated.
    info->memsize = 0;

    info->frames = (VALUE *)malloc(sizeof(VALUE) * num);
    MEMCPY(info->frames, _stackprof.frames_buffer, VALUE, num);

    info->lines_buffer = (int *)malloc(sizeof(int) * num);
    MEMCPY(info->lines_buffer, _stackprof.lines_buffer, int, num);

    if (!replace) {
        HASH_ADD(hh, _stackprof.frames_heap_live, obj, sizeof(VALUE), info);
    }
    else {
        HASH_REPLACE(hh, _stackprof.frames_heap_live, obj, sizeof(VALUE), info, tmp);
    }
}


static void
stackprof_freeobj_handler_heap(VALUE tpval, void *data)
{
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE obj = rb_tracearg_object(tparg);
    allocation_info_t *info = NULL;

    HASH_FIND(hh, _stackprof.frames_heap_live, &obj, sizeof(VALUE), info);
    if (info) {
        // If we are tracking all heap allocations, don't free the
        // allocation info.
        if (_stackprof.heap_all)
        {
            info->memsize = 0; //get_object_size(obj);
            info->living = 0;
        }
        else
        {
            HASH_DEL(_stackprof.frames_heap_live, info);

            free(info->frames);
            free(info->lines_buffer);
            free(info);

            // We need to treat this as if we didn't really sample this
            _stackprof.overall_signals--;
            _stackprof.overall_samples--;
        }
    }
}

static VALUE
stackprof_sample(VALUE self)
{
    if (!_stackprof.running)
	return Qfalse;

    _stackprof.overall_signals++;
    stackprof_job_handler(0);
    return Qtrue;
}

static int
frame_mark_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE frame = (VALUE)key;
    rb_gc_mark(frame);
    return ST_CONTINUE;
}

static void
stackprof_gc_mark(void *data)
{
    allocation_info_t *info, *tmp;
    int i;

    if (RTEST(_stackprof.out))
	rb_gc_mark(_stackprof.out);

    if (_stackprof.frames)
	st_foreach(_stackprof.frames, frame_mark_i, 0);

    if (_stackprof.frames_heap_live) {
        HASH_ITER(hh, _stackprof.frames_heap_live, info, tmp) {
            if (info->frames) {
                for (i = 0; i < info->num; i++) {
                    rb_gc_mark(info->frames[i]);
                }
            }
        }
        //st_foreach(_stackprof.frames_heap_live, heap_frame_mark_i, 0);
    }
}

static void
stackprof_atfork_prepare(void)
{
    struct itimerval timer;
    if (_stackprof.running) {
	if (_stackprof.mode == sym_wall || _stackprof.mode == sym_cpu) {
	    memset(&timer, 0, sizeof(timer));
	    setitimer(_stackprof.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
	}
    }
}

static void
stackprof_atfork_parent(void)
{
    struct itimerval timer;
    if (_stackprof.running) {
	if (_stackprof.mode == sym_wall || _stackprof.mode == sym_cpu) {
	    timer.it_interval.tv_sec = 0;
	    timer.it_interval.tv_usec = NUM2LONG(_stackprof.interval);
	    timer.it_value = timer.it_interval;
	    setitimer(_stackprof.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
	}
    }
}

static void
stackprof_atfork_child(void)
{
    allocation_info_t *info, *tmp;

    if (_stackprof.running) {
        if (_stackprof.mode == sym_heap) {
            HASH_ITER(hh, _stackprof.frames_heap_live, info, tmp) {
                HASH_DEL(_stackprof.frames_heap_live, info);

                free(info->frames);
                free(info->lines_buffer);
                free(info);
            }
        }
    }

    stackprof_stop(rb_mStackProf);
}

void
Init_stackprof(void)
{
    VALUE gc_constant;
#define S(name) sym_##name = ID2SYM(rb_intern(#name));
    S(object);
    S(custom);
    S(wall);
    S(cpu);
    S(heap);
    S(name);
    S(file);
    S(line);
    S(total_samples);
    S(gc_samples);
    S(missed_samples);
    S(samples);
    S(edges);
    S(lines);
    S(version);
    S(mode);
    S(interval);
    S(raw);
    S(out);
    S(frames);
    S(aggregate);
    S(heap_all);
#undef S

    gc_hook = Data_Wrap_Struct(rb_cObject, stackprof_gc_mark, NULL, &_stackprof);
    rb_global_variable(&gc_hook);

    rb_mStackProf = rb_define_module("StackProf");
    rb_define_singleton_method(rb_mStackProf, "running?", stackprof_running_p, 0);
    rb_define_singleton_method(rb_mStackProf, "run", stackprof_run, -1);
    rb_define_singleton_method(rb_mStackProf, "start", stackprof_start, -1);
    rb_define_singleton_method(rb_mStackProf, "stop", stackprof_stop, 0);
    rb_define_singleton_method(rb_mStackProf, "results", stackprof_results, -1);
    rb_define_singleton_method(rb_mStackProf, "sample", stackprof_sample, 0);

    // For Ruby <= 2.1.*, RVALUE size is not included when computing
    // memsize_of(obj), so we will add it explicitly.
    // TODO: is there a better way to check ruby version from the native
    // gem?
    rvalue_size = 0;
    if (ruby_version[0] <= '2' && ruby_version[2] <= '1')
    {
        gc_constant = rb_const_get(rb_mGC, rb_intern("INTERNAL_CONSTANTS"));
        rvalue_size = NUM2SIZET(rb_hash_aref(gc_constant, ID2SYM(rb_intern("RVALUE_SIZE"))));
    }

    pthread_atfork(stackprof_atfork_prepare, stackprof_atfork_parent, stackprof_atfork_child);
}
