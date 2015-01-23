## stackprofx

Stackprofx is a fork of the sampling call-stack profiler for Ruby 2.1+,
[`stackprof`][1]. It exists only because the fork's author wanted to
make some terrible changes to the upstream project that no maintainer
should accept.

### Why "x"?

I was considering calling it stackprof2, but 2 implies a successor to 1
and this is in no way "better" than the original project.

[1]: https://github.com/tmm1/stackprof

### What changes?

`StackProf::run` accepts an options hash to specify profiling parameters.
Stackprofx adds one more (optional) key: `:threads`. The value for `:threads`
should be an array of [`Thread`][1] objects to be profiled. All other threads
will be ignored.

Even if the `:threads` key is not specified, the behaviour of Stackprofx is
slightly different. `stackprof` makes use of the `rb_profile_frames()` function
added to MRI 2.1, but this thread is [limited][2] to only profiling whatever
happens to be the "current thread" when the profiling signal is received by
the Ruby process. Stackprofx will profile every running thread.

To do this, Stackprofx pulls in a bunch of private Ruby headers and reimplements
(copypasta) this function with one additional parameter: thread. This might
(probably will) break in the future, but it's for development, not production,
right?

[1]: http://ruby-doc.org/core-2.1.5/Thread.html
[2]: https://bugs.ruby-lang.org/issues/10602

### TODO

* Investigate terrible hacks required to link against Ruby
* Make stack deduplication (weights) useful again
* Less mess
* Ask someone who knows something about the Ruby GC to review
