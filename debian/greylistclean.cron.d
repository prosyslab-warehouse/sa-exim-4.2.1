# If you have configured spamd to run as a fixed user, change "Debian-exim" below.
# Be smart and don't run this as root, it doesn't need those perms
33 * * * * Debian-exim [ -x /usr/share/sa-exim/greylistclean ] && /usr/share/sa-exim/greylistclean
