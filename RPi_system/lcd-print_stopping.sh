#! /bin/sh
### BEGIN INIT INFO
# Provides:          lcdstopmessage
# Required-Start:
# Required-Stop:
# Default-Start:
# Default-Stop:      0
# Short-Description: Print the shutdown message on LCD
# Description:
### END INIT INFO

PATH=/sbin:/usr/sbin:/bin:/usr/bin

do_stop () {
	lcd-message Stopping...
	/home/pi/knc-asic/stop_all_dcdcs
}

case "$1" in
  start)
	# No-op
	;;
  restart|reload|force-reload)
	echo "Error: argument '$1' not supported" >&2
	exit 3
	;;
  stop)
	do_stop
	;;
  *)
	echo "Usage: $0 start|stop" >&2
	exit 3
	;;
esac

