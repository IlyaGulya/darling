#!/usr/bin/env bash
set -euo pipefail

readonly portable_ruby_version="4.0.5_1"
readonly ruby="/usr/local/Homebrew/Library/Homebrew/vendor/portable-ruby/${portable_ruby_version}/bin/ruby"
readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -x "$ruby" ]]; then
	echo "missing Homebrew portable Ruby ${portable_ruby_version}: $ruby" >&2
	exit 77
fi

exec "$ruby" "$script_dir/ruby_thread_kill_read.rb"
