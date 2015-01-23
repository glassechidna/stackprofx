Gem::Specification.new do |s|
  s.name = 'stackprofx'
  s.version = '0.2.9'
  s.homepage = 'https://github.com/glassechidna/stackprofx'

  s.authors = ['Aman Gupta', 'Aidan Steele']
  s.email   = ['aman@tmm1.net', 'aidan.steele@glassechidna.com.au']

  s.files = `git ls-files`.split("\n")
  s.extensions = 'ext/extconf.rb'

  s.summary = 'fork of sampling callstack-profiler for ruby 2.1+'
  s.description = 'stackprofx is a hacky derivative of stackprof, the sampling profiler for Ruby 2.1+.'

  s.license = 'MIT'

  s.add_development_dependency 'rake-compiler', '~> 0.9'
  s.add_development_dependency 'mocha', '~> 0.14'
  s.add_development_dependency 'minitest', '~> 5.0'
end
