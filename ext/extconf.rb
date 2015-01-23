require 'mkmf'
if have_func('rb_postponed_job_register_one') &&
   have_func('rb_profile_frames') &&
   have_func('rb_tracepoint_new') &&
   have_const('RUBY_INTERNAL_EVENT_NEWOBJ')

  ext_path = File.expand_path '../ruby_headers/215', __FILE__
  $CFLAGS += " -I#{ext_path}"
  create_makefile('stackprofx')
else
  fail 'missing API: are you using ruby 2.1+?'
end
