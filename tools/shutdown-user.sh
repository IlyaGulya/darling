#!/bin/sh

DARLING="${1:-darling}"

if [ "$(whoami)" != "root" ]
then
	echo "$0 must be run as root, invoking sudo"
	exec sudo "$0" "$DARLING"
fi

echo "Seeing if Darling is currently running"

# Sometimes `pgrep launchd` doesn't work when `pgrep -f launchd` does. Not sure
# if we want to include arguments (and capture rare, but possible false
# positives).
PID=$(pgrep -f launchd)

while [ -n "$PID" ]
do
	THISPID=$(echo $PID | head -n1)
	RUNNING_USER=$(ps -o uname= -p $THISPID | head -n1)
	echo "Darling currently running for $RUNNING_USER, shutting it down..."
	if echo "$DARLING" | grep -q /
	then
		DARLING_DIR=$(dirname "$DARLING")
		su --login "$RUNNING_USER" -c "PATH='$DARLING_DIR':\$PATH darling shutdown"
	else
		su --login "$RUNNING_USER" -c "$DARLING shutdown"
	fi
	sleep 2
	PID=$(pgrep -f launchd)
done

echo "No instances running now"
