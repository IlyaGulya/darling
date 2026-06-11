# frozen_string_literal: true

STDOUT.sync = true
STDERR.sync = true

100.times do |i|
  r, w = IO.pipe
  thread = Thread.new { r.read }

  sleep 0.01
  thread.kill
  raise "thread kill timed out at iteration #{i}" unless thread.join(2)

  r.close
  w.close

  STDERR.puts "iter=#{i}" if (i % 10).zero?
end

puts "ruby thread kill interrupted read"
