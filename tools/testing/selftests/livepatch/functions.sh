#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>

# Shell functions for the rest of the scripts.

MAX_RETRIES=600
RETRY_INTERVAL=".1"	# seconds

# log(msg) - write message to kernel log
#	msg - insightful words
function log() {
	echo "$1" > /dev/kmsg
}

# die(msg) - game over, man
#	msg - dying words
function die() {
	log "ERROR: $1"
	echo "ERROR: $1" >&2
	exit 1
}

# set_dynamic_debug() - setup kernel dynamic debug
#	TODO - push and pop this config?
function set_dynamic_debug() {
	cat << EOF > /sys/kernel/debug/dynamic_debug/control
file kernel/livepatch/* +p
func klp_try_switch_task -p
EOF
}

# loop_until(cmd) - loop a command until it is successful or $MAX_RETRIES,
#		    sleep $RETRY_INTERVAL between attempts
#	cmd - command and its arguments to run
function loop_until() {
	local cmd="$*"
	local i=0
	while true; do
		eval "$cmd" && return 0
		[[ $((i++)) -eq $MAX_RETRIES ]] && return 1
		sleep $RETRY_INTERVAL
	done
}

function is_livepatch_mod() {
	local mod="$1"

	if [[ $(modinfo "$mod" | awk '/^livepatch:/{print $NF}') == "Y" ]]; then
		return 0
	fi

	return 1
}

function __load_mod() {
	local mod="$1"; shift
	local args="$*"

	local msg="% modprobe $mod $args"
	log "${msg%% }"
	ret=$(modprobe "$mod" "$args" 2>&1)
	if [[ "$ret" != "" ]]; then
		die "$ret"
	fi

	# Wait for module in sysfs ...
	loop_until '[[ -e "/sys/module/$mod" ]]' ||
		die "failed to load module $mod"
}


# load_mod(modname, params) - load a kernel module
#	modname - module name to load
#	params  - module parameters to pass to modprobe
function load_mod() {
	local mod="$1"; shift
	local args="$*"

	is_livepatch_mod "$mod" &&
		die "use load_lp() to load the livepatch module $mod"

	__load_mod "$mod" "$args"
}

# load_lp_nowait(modname, params) - load a kernel module with a livepatch
#			but do not wait on until the transition finishes
#	modname - module name to load
#	params  - module parameters to pass to modprobe
function load_lp_nowait() {
	local mod="$1"; shift
	local args="$*"

	is_livepatch_mod "$mod" ||
		die "module $mod is not a livepatch"

	__load_mod "$mod" "$args"

	# Wait for livepatch in sysfs ...
	loop_until '[[ -e "/sys/kernel/livepatch/$mod" ]]' ||
		die "failed to load module $mod (sysfs)"
}

# load_lp(modname, params) - load a kernel module with a livepatch
#	modname - module name to load
#	params  - module parameters to pass to modprobe
function load_lp() {
	local mod="$1"; shift
	local args="$*"

	load_lp_nowait "$mod" "$args"

	# Wait until the transition finishes ...
	loop_until 'grep -q '^0$' /sys/kernel/livepatch/$mod/transition' ||
		die "failed to complete transition"
}

# load_failing_mod(modname, params) - load a kernel module, expect to fail
#	modname - module name to load
#	params  - module parameters to pass to modprobe
function load_failing_mod() {
	local mod="$1"; shift
	local args="$*"

	local msg="% modprobe $mod $args"
	log "${msg%% }"
	ret=$(modprobe "$mod" "$args" 2>&1)
	if [[ "$ret" == "" ]]; then
		die "$mod unexpectedly loaded"
	fi
	log "$ret"
}

# unload_mod(modname) - unload a kernel module
#	modname - module name to unload
function unload_mod() {
	local mod="$1"

	# Wait for module reference count to clear ...
	loop_until '[[ $(cat "/sys/module/$mod/refcnt") == "0" ]]' ||
		die "failed to unload module $mod (refcnt)"

	log "% rmmod $mod"
	ret=$(rmmod "$mod" 2>&1)
	if [[ "$ret" != "" ]]; then
		die "$ret"
	fi

	# Wait for module in sysfs ...
	loop_until '[[ ! -e "/sys/module/$mod" ]]' ||
		die "failed to unload module $mod (/sys/module)"
}

# unload_lp(modname) - unload a kernel module with a livepatch
#	modname - module name to unload
function unload_lp() {
	unload_mod "$1"
}

# disable_lp(modname) - disable a livepatch
#	modname - module name to unload
function disable_lp() {
	local mod="$1"

	log "% echo 0 > /sys/kernel/livepatch/$mod/enabled"
	echo 0 > /sys/kernel/livepatch/"$mod"/enabled

	# Wait until the transition finishes and the livepatch gets
	# removed from sysfs...
	loop_until '[[ ! -e "/sys/kernel/livepatch/$mod" ]]' ||
		die "failed to disable livepatch $mod"
}

# set_pre_patch_ret(modname, pre_patch_ret)
#	modname - module name to set
#	pre_patch_ret - new pre_patch_ret value
function set_pre_patch_ret {
	local mod="$1"; shift
	local ret="$1"

	log "% echo $ret > /sys/module/$mod/parameters/pre_patch_ret"
	echo "$ret" > /sys/module/"$mod"/parameters/pre_patch_ret

	# Wait for sysfs value to hold ...
	loop_until '[[ $(cat "/sys/module/$mod/parameters/pre_patch_ret") == "$ret" ]]' ||
		die "failed to set pre_patch_ret parameter for $mod module"
}

# check_result() - verify dmesg output
#	TODO - better filter, out of order msgs, etc?
function check_result {
	local expect="$*"
	local result

	result=$(dmesg | grep -v 'tainting' | grep -e 'livepatch:' -e 'test_klp' | sed 's/^\[[ 0-9.]*\] //')

	if [[ "$expect" == "$result" ]] ; then
		echo "ok"
	else
		echo -e "not ok\n\n$(diff -upr --label expected --label result <(echo "$expect") <(echo "$result"))\n"
		die "livepatch kselftest(s) failed"
	fi
}
