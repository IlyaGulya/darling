# frozen_string_literal: true

class InterruptThread < StandardError
end

STDOUT.sync = true
STDERR.sync = true

100.times do |i|
  r, w = IO.pipe
  ready = Queue.new

  thread = Thread.new do
    Thread.handle_interrupt(InterruptThread => :never) do
      ready << true
      begin
        Thread.handle_interrupt(InterruptThread => :on_blocking) do
          IO.select([r])
        end
      rescue InterruptThread
      end
    end
  end

  ready.pop
  thread.raise InterruptThread
  thread.join
  r.close
  w.close

  STDERR.puts "iter=#{i}" if (i % 10).zero?
end

puts "ruby thread raise select interrupted"
