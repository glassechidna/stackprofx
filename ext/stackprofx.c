/**********************************************************************

  stackprofx.c - Sampling call-stack frame profiler for MRI.

  vim: setl noexpandtab shiftwidth=4 tabstop=8 softtabstop=4

**********************************************************************/

#include <ruby/ruby.h>
#include <ruby/debug.h>
#include <ruby/st.h>
#include <ruby/io.h>
#include <ruby/intern.h>
#include <signal.h>
#include <sys/time.h>
#include <pthread.h>

#define ruby_current_thread ((rb_thread_t *)RTYPEDDATA_DATA(rb_thread_current()))

#include "vm_core.h"
#include "iseq.h"

static inline const rb_data_type_t *
threadptr_data_type(void)
{
    static const rb_data_type_t *thread_data_type;
    if (!thread_data_type)
    {
        VALUE current_thread = rb_thread_current();
        thread_data_type = RTYPEDDATA_TYPE(current_thread);
    }
    return thread_data_type;
}

#define ruby_thread_data_type *threadptr_data_type()
#define ruby_threadptr_data_type *threadptr_data_type()

#define BUF_SIZE 2048

typedef struct
{
    size_t total_samples;
    size_t caller_samples;
    st_table *lines;
} frame_data_t;

typedef struct
{
    int running;

    VALUE mode;
    VALUE interval;

    VALUE *raw_samples;
    size_t raw_samples_len;
    size_t raw_samples_capa;
    size_t raw_sample_index;

    size_t overall_signals;
    size_t overall_samples;
    size_t during_gc;
    st_table *frames;

    st_table *threads;

    VALUE frames_buffer[BUF_SIZE];
    int lines_buffer[BUF_SIZE];
} stackprofx_t;

static stackprofx_t _stackprofx;

static VALUE sym_object, sym_wall, sym_cpu, sym_custom, sym_name, sym_file, sym_line, sym_threads;
static VALUE sym_samples, sym_total_samples, sym_missed_samples, sym_lines;
static VALUE sym_version, sym_mode, sym_interval, sym_raw, sym_frames;
static VALUE sym_gc_samples, objtracer;
static VALUE gc_hook;
static VALUE rb_mStackProfx;

static void stackprofx_newobj_handler(VALUE, void *);

static void stackprofx_signal_handler(int sig, siginfo_t *sinfo, void *ucontext);

static VALUE
stackprofx_start(int argc, VALUE *argv, VALUE self)
{
    struct sigaction sa;
    struct itimerval timer;
    VALUE opts = Qnil, mode = Qnil, interval = Qnil, threads = Qnil;

    if (_stackprofx.running) return Qfalse;

    rb_scan_args(argc, argv, "0:", &opts);

    if (RTEST(opts))
    {
        mode = rb_hash_aref(opts, sym_mode);
        interval = rb_hash_aref(opts, sym_interval);
        threads = rb_hash_aref(opts, sym_threads);
    }
    if (!RTEST(mode)) mode = sym_wall;

    if (RTEST(threads))
    {
        _stackprofx.threads = st_init_numtable();
        for (int i = 0; i < RARRAY_LEN(threads); i++)
        {
            VALUE thr = rb_ary_entry(threads, i);
            st_add_direct(_stackprofx.threads, thr, 0);
            rb_gc_mark(thr);
        }
    }
    else
    {
        _stackprofx.threads = 0;
    }

    if (!_stackprofx.frames)
    {
        _stackprofx.frames = st_init_numtable();
        _stackprofx.overall_signals = 0;
        _stackprofx.overall_samples = 0;
        _stackprofx.during_gc = 0;
    }

    if (mode == sym_object)
    {
        if (!RTEST(interval)) interval = INT2FIX(1);

        objtracer = rb_tracepoint_new(Qnil, RUBY_INTERNAL_EVENT_NEWOBJ, stackprofx_newobj_handler, 0);
        rb_tracepoint_enable(objtracer);
    }
    else if (mode == sym_wall || mode == sym_cpu)
    {
        if (!RTEST(interval)) interval = INT2FIX(1000);

        sa.sa_sigaction = stackprofx_signal_handler;
        sa.sa_flags = SA_RESTART | SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(mode == sym_wall ? SIGALRM : SIGPROF, &sa, NULL);

        timer.it_interval.tv_sec = 0;
        timer.it_interval.tv_usec = NUM2LONG(interval);
        timer.it_value = timer.it_interval;
        setitimer(mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
    }
    else if (mode == sym_custom)
    {
        /* sampled manually */
        interval = Qnil;
    } else
    {
        rb_raise(rb_eArgError, "unknown profiler mode");
    }

    _stackprofx.running = 1;
    _stackprofx.mode = mode;
    _stackprofx.interval = interval;

    return Qtrue;
}

static VALUE
stackprofx_stop(VALUE self)
{
    struct sigaction sa;
    struct itimerval timer;

    if (!_stackprofx.running) return Qfalse;
    _stackprofx.running = 0;

    if (_stackprofx.threads)
    {
        st_free_table(_stackprofx.threads);
        _stackprofx.threads = 0;
    }

    if (_stackprofx.mode == sym_object)
    {
        rb_tracepoint_disable(objtracer);
    }
    else if (_stackprofx.mode == sym_wall || _stackprofx.mode == sym_cpu)
    {
        memset(&timer, 0, sizeof(timer));
        setitimer(_stackprofx.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);

        sa.sa_handler = SIG_IGN;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(_stackprofx.mode == sym_wall ? SIGALRM : SIGPROF, &sa, NULL);
    }
    else if (_stackprofx.mode == sym_custom)
    {
        /* sampled manually */
    } else
    {
        rb_raise(rb_eArgError, "unknown profiler mode");
    }

    return Qtrue;
}

static int
frame_lines_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE lines = (VALUE)arg;

    size_t weight = (size_t)val;
    size_t total = weight & (~(size_t)0 << (8 * SIZEOF_SIZE_T / 2));
    weight -= total;
    total = total >> (8 * SIZEOF_SIZE_T / 2);
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
    VALUE name, file, lines;
    VALUE line;

    rb_hash_aset(results, rb_obj_id(frame), details);

    name = rb_profile_frame_full_label(frame);
    rb_hash_aset(details, sym_name, name);

    file = rb_profile_frame_absolute_path(frame);
    if (NIL_P(file)) file = rb_profile_frame_path(frame);
    rb_hash_aset(details, sym_file, file);

    if ((line = rb_profile_frame_first_lineno(frame)) != INT2FIX(0)) rb_hash_aset(details, sym_line, line);

    rb_hash_aset(details, sym_total_samples, SIZET2NUM(frame_data->total_samples));
    rb_hash_aset(details, sym_samples, SIZET2NUM(frame_data->caller_samples));

    if (frame_data->lines)
    {
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
stackprofx_results(VALUE self)
{
    VALUE results, frames;

    if (!_stackprofx.frames || _stackprofx.running) return Qnil;

    results = rb_hash_new();
    rb_hash_aset(results, sym_version, DBL2NUM(1.1));
    rb_hash_aset(results, sym_mode, _stackprofx.mode);
    rb_hash_aset(results, sym_interval, _stackprofx.interval);
    rb_hash_aset(results, sym_samples, SIZET2NUM(_stackprofx.overall_samples));
    rb_hash_aset(results, sym_gc_samples, SIZET2NUM(_stackprofx.during_gc));
    rb_hash_aset(results, sym_missed_samples, SIZET2NUM(_stackprofx.overall_signals - _stackprofx.overall_samples));

    frames = rb_hash_new();
    rb_hash_aset(results, sym_frames, frames);
    st_foreach(_stackprofx.frames, frame_i, (st_data_t)frames);

    st_free_table(_stackprofx.frames);
    _stackprofx.frames = NULL;

    if (_stackprofx.raw_samples_len > 0)
    {
        size_t len, n, o;
        VALUE raw_samples = rb_ary_new_capa(_stackprofx.raw_samples_len);

        for (n = 0; n < _stackprofx.raw_samples_len; n++)
        {
            len = (size_t)_stackprofx.raw_samples[n];
            rb_ary_push(raw_samples, SIZET2NUM(len));

            for (o = 0, n++; o < len; n++, o++)
                rb_ary_push(raw_samples, rb_obj_id(_stackprofx.raw_samples[n]));
            rb_ary_push(raw_samples, SIZET2NUM((size_t)_stackprofx.raw_samples[n]));
        }

        free(_stackprofx.raw_samples);
        _stackprofx.raw_samples = NULL;
        _stackprofx.raw_samples_len = 0;
        _stackprofx.raw_samples_capa = 0;
        _stackprofx.raw_sample_index = 0;

        rb_hash_aset(results, sym_raw, raw_samples);
    }

    return results;
}

static VALUE
stackprofx_run(int argc, VALUE *argv, VALUE self)
{
    rb_need_block();
    stackprofx_start(argc, argv, self);
    rb_ensure(rb_yield, Qundef, stackprofx_stop, self);
    return stackprofx_results(self);
}

static VALUE
stackprofx_running_p(VALUE self)
{
    return _stackprofx.running ? Qtrue : Qfalse;
}

static inline frame_data_t *
sample_for(VALUE frame)
{
    st_data_t key = (st_data_t)frame, val = 0;
    frame_data_t *frame_data;

    if (st_lookup(_stackprofx.frames, key, &val))
    {
        frame_data = (frame_data_t *)val;
    }
    else
    {
        frame_data = ALLOC_N(frame_data_t, 1);
        MEMZERO(frame_data, frame_data_t, 1);
        val = (st_data_t)frame_data;
        st_insert(_stackprofx.frames, key, val);
    }

    return frame_data;
}

static int
numtable_increment_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing)
{
    size_t *weight = (size_t *)value;
    size_t increment = (size_t)arg;

    if (existing)
    {
        (*weight) += increment;
    }
    else
    {
        *weight = increment;
    }

    return ST_CONTINUE;
}

void
st_numtable_increment(st_table *table, st_data_t key, size_t increment)
{
    st_update(table, key, numtable_increment_callback, (st_data_t)increment);
}

// thanks to https://bugs.ruby-lang.org/issues/10602
int
rb_profile_frames_thread(int start, int limit, VALUE *buff, int *lines, rb_thread_t *th)
{
    int i;
    rb_control_frame_t *cfp = th->cfp, *end_cfp = RUBY_VM_END_CONTROL_FRAME(th);

    for (i = 0; i < limit && cfp != end_cfp;)
    {
        if (cfp->iseq && cfp->pc)
        { /* should be NORMAL_ISEQ */
            if (start > 0)
            {
                start--;
                continue;
            }

            /* record frame info */
            buff[i] = cfp->iseq->self;
            if (lines) lines[i] = rb_iseq_line_no(cfp->iseq, cfp->pc - cfp->iseq->iseq_encoded);
            i++;
        }
        cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }

    return i;
}

void
stackprofx_resize_raw_samples(int num)
{
    if (!_stackprofx.raw_samples)
    {
        _stackprofx.raw_samples_capa = num * 100;
        _stackprofx.raw_samples = malloc(sizeof(VALUE) * _stackprofx.raw_samples_capa);
    }

    if (_stackprofx.raw_samples_capa <= _stackprofx.raw_samples_len + num)
    {
        _stackprofx.raw_samples_capa *= 2;
        _stackprofx.raw_samples = realloc(_stackprofx.raw_samples, sizeof(VALUE) * _stackprofx.raw_samples_capa);
    }
}

int
stackprofx_same_stackframe_as_prev(int num)
{
    int i, n;

    if (_stackprofx.raw_samples_len > 0 && _stackprofx.raw_samples[_stackprofx.raw_sample_index] == (VALUE)num)
    {
        for (i = num - 1, n = 0; i >= 0; i--, n++)
        {
            VALUE frame = _stackprofx.frames_buffer[i];
            if (_stackprofx.raw_samples[_stackprofx.raw_sample_index + 1 + n] != frame) return 0;
        }
        if (i == -1) return 1;
    }

    return 0;
}

int
stackprofx_record_sample_i(st_data_t key, st_data_t val, st_data_t arg)
{
    int num, i, n;
    VALUE prev_frame = Qnil;

    rb_thread_t *th;
    GetThreadPtr((VALUE)key, th);
    if (th->status != THREAD_RUNNABLE) return ST_CONTINUE;

    num = rb_profile_frames_thread(
        0,
        sizeof(_stackprofx.frames_buffer) / sizeof(VALUE),
        _stackprofx.frames_buffer,
        _stackprofx.lines_buffer,
        th
    );

    stackprofx_resize_raw_samples(num);

    if (stackprofx_same_stackframe_as_prev(num))
    {
        _stackprofx.raw_samples[_stackprofx.raw_samples_len - 1] += 1;
    }
    else
    {
        _stackprofx.raw_sample_index = _stackprofx.raw_samples_len;
        _stackprofx.raw_samples[_stackprofx.raw_samples_len++] = (VALUE)num;
        for (i = num - 1; i >= 0; i--)
        {
            VALUE frame = _stackprofx.frames_buffer[i];
            _stackprofx.raw_samples[_stackprofx.raw_samples_len++] = frame;
        }
        _stackprofx.raw_samples[_stackprofx.raw_samples_len++] = (VALUE)1;
    }

    for (i = 0; i < num; i++)
    {
        int line = _stackprofx.lines_buffer[i];
        VALUE frame = _stackprofx.frames_buffer[i];
        frame_data_t *frame_data = sample_for(frame);

        frame_data->total_samples++;

        if (i == 0) frame_data->caller_samples++;

        if (line > 0)
        {
            if (!frame_data->lines) frame_data->lines = st_init_numtable();
            size_t half = (size_t)1 << (8 * SIZEOF_SIZE_T / 2);
            size_t increment = i == 0 ? half + 1 : half;
            st_numtable_increment(frame_data->lines, (st_data_t)line, increment);
        }

        prev_frame = frame;
    }

    return ST_CONTINUE;
}

void
stackprofx_record_sample()
{
    _stackprofx.overall_samples++;
    st_table *tbl = _stackprofx.threads ?: GET_THREAD()->vm->living_threads;
    st_foreach(tbl, stackprofx_record_sample_i, 0);
}

static void
stackprofx_job_handler(void *data)
{
    static int in_signal_handler = 0;
    if (in_signal_handler) return;
    if (!_stackprofx.running) return;

    in_signal_handler++;
    stackprofx_record_sample();
    in_signal_handler--;
}

static void
stackprofx_signal_handler(int sig, siginfo_t *sinfo, void *ucontext)
{
    _stackprofx.overall_signals++;
    if (rb_during_gc())
    {
        _stackprofx.during_gc++, _stackprofx.overall_samples++;
    }
    else
    {
        rb_postponed_job_register_one(0, stackprofx_job_handler, 0);
    }
}

static void
stackprofx_newobj_handler(VALUE tpval, void *data)
{
    _stackprofx.overall_signals++;
    if (RTEST(_stackprofx.interval) && _stackprofx.overall_signals % NUM2LONG(_stackprofx.interval)) return;
    stackprofx_job_handler(0);
}

static VALUE
stackprofx_sample(VALUE self)
{
    if (!_stackprofx.running) return Qfalse;

    _stackprofx.overall_signals++;
    stackprofx_job_handler(0);
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
stackprofx_gc_mark(void *data)
{
    if (_stackprofx.frames) st_foreach(_stackprofx.frames, frame_mark_i, 0);
}

static void
stackprofx_atfork_prepare(void)
{
    struct itimerval timer;
    if (_stackprofx.running)
    {
        if (_stackprofx.mode == sym_wall || _stackprofx.mode == sym_cpu)
        {
            memset(&timer, 0, sizeof(timer));
            setitimer(_stackprofx.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
        }
    }
}

static void
stackprofx_atfork_parent(void)
{
    struct itimerval timer;
    if (_stackprofx.running)
    {
        if (_stackprofx.mode == sym_wall || _stackprofx.mode == sym_cpu)
        {
            timer.it_interval.tv_sec = 0;
            timer.it_interval.tv_usec = NUM2LONG(_stackprofx.interval);
            timer.it_value = timer.it_interval;
            setitimer(_stackprofx.mode == sym_wall ? ITIMER_REAL : ITIMER_PROF, &timer, 0);
        }
    }
}

static void
stackprofx_atfork_child(void)
{
    stackprofx_stop(rb_mStackProfx);
}

void
Init_stackprofx(void)
{
#define S(name) sym_##name = ID2SYM(rb_intern(#name));
    S(object);
    S(custom);
    S(wall);
    S(threads);
    S(cpu);
    S(name);
    S(file);
    S(line);
    S(total_samples);
    S(gc_samples);
    S(missed_samples);
    S(samples);
    S(lines);
    S(version);
    S(mode);
    S(interval);
    S(frames);
#undef S

    gc_hook = Data_Wrap_Struct(rb_cObject, stackprofx_gc_mark, NULL, NULL);
    rb_global_variable(&gc_hook);

    rb_mStackProfx = rb_define_module("StackProfx");
    rb_define_singleton_method(rb_mStackProfx, "running?", stackprofx_running_p, 0);
    rb_define_singleton_method(rb_mStackProfx, "run", stackprofx_run, -1);
    rb_define_singleton_method(rb_mStackProfx, "start", stackprofx_start, -1);
    rb_define_singleton_method(rb_mStackProfx, "stop", stackprofx_stop, 0);
    rb_define_singleton_method(rb_mStackProfx, "results", stackprofx_results, 0);
    rb_define_singleton_method(rb_mStackProfx, "sample", stackprofx_sample, 0);

    pthread_atfork(stackprofx_atfork_prepare, stackprofx_atfork_parent, stackprofx_atfork_child);
}
