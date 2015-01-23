$:.unshift File.expand_path('../lib', __FILE__)
require 'stackprofx'

class A
  def initialize
    pow
    self.class.newobj
    math
  end

  def pow
    2 ** 100
  end

  def self.newobj
    Object.new
    Object.new
  end

  def math
    2.times do
      2 + 3 * 4 ^ 5 / 6
    end
  end
end

#profile = StackProfx.run(:object, 1) do
#profile = StackProfx.run(:wall, 1000) do
profile = StackProfx.run(:cpu, 1000) do
  1_000_000.times do
    A.new
  end
end

