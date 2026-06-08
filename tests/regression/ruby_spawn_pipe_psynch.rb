# frozen_string_literal: true

# Regression for Darling psynch cvwait validation. Homebrew portable Ruby
# 4.0.5_1 could spin forever in Ruby/Homebrew process handling when
# __psynch_cvwait rejected cvlsgen states where S == L. This reproducer is
# distilled from Homebrew's Ruby path that waits for child processes while
# other Ruby threads read subprocess pipes.
#
# Original minimized Ruby reproducer:
#
#   threads = Integer(ENV.fetch("THREADS"))
#   iters = Integer(ENV.fetch("ITERS", "100"))
#   mode = ENV.fetch("MODE", "stdoutfirst")
#   threads.times.map do |t|
#     Thread.new do
#       iters.times do |i|
#         r, w = IO.pipe
#         er, ew = IO.pipe
#         pid = spawn("/bin/echo", "worker=#{t}", "iter=#{i}", out: w, err: ew)
#         ew.close
#         w.close
#         if mode == "errfirst"
#           err = er.read; er.close
#           out = r.read; r.close
#         else
#           out = r.read; r.close
#           err = er.read; er.close
#         end
#         _, st = Process.wait2(pid)
#         raise "bad status" unless st.success?
#         raise "bad output #{out.inspect}" unless out.include?("worker=#{t}")
#         raise "stderr not empty: #{err.inspect}" unless err.empty?
#       end
#     end
#   end.each(&:join)

STDOUT.sync = true
STDERR.sync = true

threads = Integer(ENV.fetch("THREADS", "4"))
iters = Integer(ENV.fetch("ITERS", "80"))
mode = ENV.fetch("MODE", "stdoutfirst")

threads.times.map do |thread_index|
  Thread.new do
    iters.times do |iteration|
      stdout_read, stdout_write = IO.pipe
      stderr_read, stderr_write = IO.pipe

      pid = spawn(
        "/bin/echo",
        "worker=#{thread_index}",
        "iter=#{iteration}",
        out: stdout_write,
        err: stderr_write,
      )

      stderr_write.close
      stdout_write.close

      if mode == "errfirst"
        stderr = stderr_read.read
        stderr_read.close
        stdout = stdout_read.read
        stdout_read.close
      else
        stdout = stdout_read.read
        stdout_read.close
        stderr = stderr_read.read
        stderr_read.close
      end

      _, status = Process.wait2(pid)
      raise "bad status for worker=#{thread_index} iter=#{iteration}" unless status.success?
      raise "bad output: #{stdout.inspect}" unless stdout.include?("worker=#{thread_index}")
      raise "stderr not empty: #{stderr.inspect}" unless stderr.empty?

      STDERR.puts "thread=#{thread_index} iteration=#{iteration}" if (iteration % 20).zero?
    end
  end
end.each(&:join)

puts "ruby spawn pipe psynch stress passed"
