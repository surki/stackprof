require 'mkmf'
if have_func('rb_postponed_job_register_one') &&
   have_func('rb_profile_frames') &&
   have_func('rb_tracepoint_new') &&
   have_func('rb_obj_memsize_of') &&
   have_const('RUBY_INTERNAL_EVENT_NEWOBJ') &&
   have_const('RUBY_INTERNAL_EVENT_FREEOBJ')
  create_makefile('stackprof/stackprof')
else
  fail 'missing API: are you using ruby 2.1+?'
end
