# frozen_string_literal: true

# Load with `ruby -r/path/to/ruby-thread-watchdog.rb ...`.
# Set RUBY_THREAD_WATCHDOG_SECONDS and optionally RUBY_THREAD_WATCHDOG_OUTPUT.
seconds = Integer(
  ENV.fetch("RUBY_THREAD_WATCHDOG_SECONDS") {
    ENV.fetch("HOMEBREW_RUBY_THREAD_WATCHDOG_SECONDS", "60")
  }
)
output_path = ENV["RUBY_THREAD_WATCHDOG_OUTPUT"] || ENV["HOMEBREW_RUBY_THREAD_WATCHDOG_OUTPUT"]

Thread.new do
  Thread.current.name = "thread-watchdog" if Thread.current.respond_to?(:name=)
  loop do
    sleep seconds
    output = output_path ? File.open(output_path, "a") : $stderr
    begin
      output.puts("=== Ruby thread dump #{Time.now} pid=#{Process.pid} ===")
      Thread.list.each do |thread|
        output.puts(
          "--- thread=#{thread.object_id} name=#{thread.name.inspect} " \
          "status=#{thread.status.inspect} current=#{thread == Thread.current}"
        )
        Array(thread.backtrace).each { |line| output.puts("  #{line}") }
      end
      output.flush
    ensure
      output.close if output_path
    end
  end
end
