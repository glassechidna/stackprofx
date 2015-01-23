$:.unshift File.expand_path('../../lib', __FILE__)
require 'stackprofx'
require 'minitest/autorun'
require 'tempfile'

class StackProfxTest < MiniTest::Test
  def test_info
    profile = StackProfx.run{}
    assert_equal 1.1, profile[:version]
    assert_equal :wall, profile[:mode]
    assert_equal 1000, profile[:interval]
    assert_equal 0, profile[:samples]
  end

  def test_running
    assert_equal false, StackProfx.running?
    StackProfx.run{ assert_equal true, StackProfx.running? }
  end

  def test_start_stop_results
    assert_equal nil, StackProfx.results
    assert_equal true, StackProfx.start
    assert_equal false, StackProfx.start
    assert_equal true, StackProfx.running?
    assert_equal nil, StackProfx.results
    assert_equal true, StackProfx.stop
    assert_equal false, StackProfx.stop
    assert_equal false, StackProfx.running?
    assert_kind_of Hash, StackProfx.results
    assert_equal nil, StackProfx.results
  end

  def test_object_allocation
    profile = StackProfx.run(mode: :object) do
      Object.new
      Object.new
    end
    assert_equal :object, profile[:mode]
    assert_equal 1, profile[:interval]
    assert_equal 2, profile[:samples]

    frame = profile[:frames].values.first
    assert_equal "block in StackProfxTest#test_object_allocation", frame[:name]
    assert_equal 2, frame[:samples]
    line = __LINE__
    assert_equal line-11, frame[:line]
    assert_equal [1, 1], frame[:lines][line-10]
    assert_equal [1, 1], frame[:lines][line-9]

    frame = profile[:frames].values[1]
    assert_equal [2, 0], frame[:lines][line-11]
  end

  def test_object_allocation_interval
    profile = StackProfx.run(mode: :object, interval: 10) do
      100.times { Object.new }
    end
    assert_equal 10, profile[:samples]
  end

  def test_cputime
    profile = StackProfx.run(mode: :cpu, interval: 500) do
      math
    end

    assert_operator profile[:samples], :>, 1
    frame = profile[:frames].values.first
    assert_equal "block in StackProfxTest#math", frame[:name]
  end

  def test_walltime
    profile = StackProfx.run(mode: :wall) do
      idle
    end

    frame = profile[:frames].values.first
    assert_equal "StackProfxTest#idle", frame[:name]
    assert_in_delta 200, frame[:samples], 5
  end

  def test_custom
    profile = StackProfx.run(mode: :custom) do
      10.times do
        StackProfx.sample
      end
    end

    assert_equal :custom, profile[:mode]
    assert_equal 10, profile[:samples]

    frame = profile[:frames].values.first
    assert_equal "block (2 levels) in StackProfxTest#test_custom", frame[:name]
    assert_equal __LINE__-10, frame[:line]
    assert_equal [10, 10], frame[:lines][__LINE__-10]
  end

  def test_raw
    profile = StackProfx.run(mode: :custom, raw: true) do
      10.times do
        StackProfx.sample
      end
    end

    raw = profile[:raw]
    assert_equal 10, raw[-1]
    assert_equal raw[0] + 2, raw.size
    assert_equal 'block (2 levels) in StackProfxTest#test_raw', profile[:frames][raw[-2]][:name]
  end

  def test_fork
    StackProfx.run do
      pid = fork do
        exit! StackProfx.running?? 1 : 0
      end
      Process.wait(pid)
      assert_equal 0, $?.exitstatus
      assert_equal true, StackProfx.running?
    end
  end

  def test_gc
    profile = StackProfx.run(interval: 100) do
      5.times do
        GC.start
      end
    end

    assert_empty profile[:frames]
    assert_operator profile[:gc_samples], :>, 0
    assert_equal 0, profile[:missed_samples]
  end

  def test_out
    tmpfile = Tempfile.new('stackprof-out')
    ret = StackProfx.run(mode: :custom, out: tmpfile) do
      StackProfx.sample
    end

    assert_equal tmpfile, ret
    tmpfile.rewind
    profile = Marshal.load(tmpfile.read)
    refute_empty profile[:frames]
  end

  def math
    250_000.times do
      2 ** 10
    end
  end

  def idle
    r, w = IO.pipe
    IO.select([r], nil, nil, 0.2)
  ensure
    r.close
    w.close
  end
end
