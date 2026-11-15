#!/usr/bin/env bash
# HyperHDR installation and diagnostics checks (issue #27).
#
#   scripts/diagnostics/hyperhdr-doctor.sh [--checks install,runtime,capture|all]
#       [--user NAME] [--userdata DIR] [--since 30min] [--led-host ADDRESS]
#       [--db-check] [--json] [--archive DIR] [--no-color]
#
# Prints one line per check: [PASS] [WARN] [FAIL] [SKIP] id: finding -> hint.
# Exit status: 0 no FAIL, 1 at least one FAIL, 2 usage error.
#
# Read-only: it never changes services, packages, groups, device settings or
# firewall rules, never opens a capture device for streaming, and never reads
# the settings database (--db-check runs a read-only PRAGMA quick_check only).
# --led-host is the only check that contacts another machine.
# docs/diagnostics.md lists every check id, its expectation and its fix.
set -u

CHECKS=all; JSON=0; ARCHIVE=""; SINCE=30min; LED_HOST=""; SVC_USER=""; USERDATA=""; DBCHECK=0; COLOR=1
usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
	case "$1" in
		--checks) CHECKS=${2:-}; shift 2;;
		--user) SVC_USER=${2:-}; shift 2;;
		--userdata) USERDATA=${2:-}; shift 2;;
		--since) SINCE=${2:-}; shift 2;;
		--led-host) LED_HOST=${2:-}; shift 2;;
		--db-check) DBCHECK=1; shift;;
		--json) JSON=1; shift;;
		--archive) ARCHIVE=${2:-}; shift 2;;
		--no-color) COLOR=0; shift;;
		-h|--help) usage; exit 0;;
		*) echo "unknown option: $1 (see --help)" >&2; exit 2;;
	esac
done
case ",$CHECKS," in *,all,*|*,install,*|*,runtime,*|*,capture,*) ;; *) echo "--checks takes install, runtime, capture or all" >&2; exit 2;; esac
[ -t 1 ] || COLOR=0

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=../lib/redact.sh
. "$HERE/../lib/redact.sh"
# Tests point R at a fixture tree; on a real system it is empty.
R=${HYPERHDR_DOCTOR_ROOT:-}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
EV="$WORK/evidence"; mkdir -p "$EV"

have() { command -v "$1" >/dev/null 2>&1; }
want() { case ",$CHECKS," in *,all,*|*,"$1",*) return 0;; esac; return 1; }
# ev <id> <command...>: keep a command's output as evidence and echo it.
ev() { local id=$1; shift; { timeout 20 "$@" 2>&1; echo "[exit $?]"; } | tee -a "$EV/$id.txt" | sed '$d'; }

IDS=(); GRPS=(); STATS=(); FINDS=(); HINTS=()
GROUP=""
check() {
	IDS+=("$1"); GRPS+=("$GROUP"); STATS+=("$2"); FINDS+=("$3"); HINTS+=("${4:-}")
	local tag=$2 c="" n=""
	if [ "$COLOR" -eq 1 ]; then
		n=$'\033[0m'
		case "$2" in PASS) c=$'\033[32m';; WARN) c=$'\033[33m';; FAIL) c=$'\033[31m';; SKIP) c=$'\033[90m';; esac
	fi
	[ "$JSON" -eq 1 ] && return
	printf '%s[%s]%s %s: %s%s\n' "$c" "$tag" "$n" "$1" "$3" "${4:+ -> $4}"
}
pass() { check "$1" PASS "$2" "${3:-}"; }
warn() { check "$1" WARN "$2" "${3:-}"; }
fail() { check "$1" FAIL "$2" "${3:-}"; }
skip() { check "$1" SKIP "$2" "${3:-}"; }

# --- who and where -----------------------------------------------------------
SVC_SOURCE="--user"
if [ -z "$SVC_USER" ] && have systemctl; then
	SVC_USER=$(systemctl list-units --all --plain --no-legend 'hyperhdr@*.service' 2>/dev/null | awk '{print $1}' | sed -n 's/^hyperhdr@\(.*\)\.service$/\1/p' | head -1)
	SVC_SOURCE="systemd unit"
fi
if [ -z "$SVC_USER" ]; then
	# Enabled but never started instances are not loaded units; read the wants links.
	SVC_USER=$(ls "$R"/etc/systemd/system/*.wants/hyperhdr@*.service 2>/dev/null | head -1 | sed -n 's/.*hyperhdr@\(.*\)\.service$/\1/p')
	[ -n "$SVC_USER" ] && SVC_SOURCE="enabled systemd unit"
fi
if [ -z "$SVC_USER" ]; then SVC_USER=$(ps -o user= -C hyperhdr 2>/dev/null | head -1 | tr -d ' '); SVC_SOURCE="running process"; fi
if [ -z "$SVC_USER" ]; then SVC_USER=$(id -un); SVC_SOURCE="current user (no service or process found)"; fi
SVC_HOME=$(getent passwd "$SVC_USER" 2>/dev/null | cut -d: -f6)
[ -z "$USERDATA" ] && USERDATA="${SVC_HOME:-$HOME}/.hyperhdr"
BIN=$(command -v hyperhdr 2>/dev/null || true)

# --- install -------------------------------------------------------------------
if want install; then
	GROUP=install
	OSR="$R/etc/os-release"
	if [ -r "$OSR" ]; then
		ID=$(sed -n 's/^ID=//p' "$OSR" | tr -d '"'); LIKE=$(sed -n 's/^ID_LIKE=//p' "$OSR" | tr -d '"')
		PRETTY=$(sed -n 's/^PRETTY_NAME=//p' "$OSR" | tr -d '"')
		case " $ID $LIKE " in
			*" debian "*|*" ubuntu "*) pass os.release "$PRETTY (deb packages)";;
			*" fedora "*|*" rhel "*) pass os.release "$PRETTY (rpm packages)";;
			*" arch "*) pass os.release "$PRETTY (Arch packages)";;
			*) warn os.release "$PRETTY" "no package is built for this distribution; build from source (BUILDING.md)";;
		esac
	else
		warn os.release "/etc/os-release not readable" "cannot tell which package type applies"
	fi

	ARCH=$(uname -m)
	case "$ARCH" in
		x86_64|aarch64|armv7l|armv6l) pass os.arch "$ARCH";;
		*) warn os.arch "$ARCH" "packages are built for x86_64, aarch64 and armhf";;
	esac

	pass kernel.version "$(uname -r)"

	if [ -d "$R/run/systemd/system" ]; then pass sys.systemd "systemd is the init system"
	else warn sys.systemd "systemd is not running" "the hyperhdr@<user> service cannot be used; start hyperhdr another way"; fi

	DF_PATH=$USERDATA; [ -e "$DF_PATH" ] || DF_PATH=${SVC_HOME:-$HOME}
	FREE_MB=$(df -Pm "$DF_PATH" 2>/dev/null | awk 'NR==2 {print $4}')
	if [ -z "$FREE_MB" ]; then skip sys.disk "free space unknown" "df failed for $DF_PATH"
	elif [ "$FREE_MB" -lt 100 ]; then fail sys.disk "${FREE_MB} MB free for $DF_PATH" "free at least 200 MB (database, logs, ~50 MB LUT)"
	elif [ "$FREE_MB" -lt 200 ]; then warn sys.disk "${FREE_MB} MB free for $DF_PATH" "200 MB or more is recommended"
	else pass sys.disk "${FREE_MB} MB free for $DF_PATH"; fi

	AVAIL_KB=$(awk '/^MemAvailable:/ {print $2}' "$R/proc/meminfo" 2>/dev/null)
	if [ -z "$AVAIL_KB" ]; then skip sys.memory "MemAvailable unknown"
	elif [ "$AVAIL_KB" -lt 262144 ]; then warn sys.memory "$((AVAIL_KB / 1024)) MB available" "HyperHDR with a full LUT needs about 256 MB"
	else pass sys.memory "$((AVAIL_KB / 1024)) MB available"; fi

	PKG_VER=""; PKG_ARCH=""
	if have dpkg-query && dpkg-query -W -f '${Status}' hyperhdr 2>/dev/null | grep -q "install ok installed"; then
		PKG_VER=$(dpkg-query -W -f '${Version}' hyperhdr); PKG_ARCH=$(dpkg-query -W -f '${Architecture}' hyperhdr)
		pass pkg.installed "hyperhdr $PKG_VER ($PKG_ARCH, dpkg)"
	elif have rpm && rpm -q hyperhdr >/dev/null 2>&1; then
		PKG_VER=$(rpm -q --qf '%{VERSION}' hyperhdr); PKG_ARCH=$(rpm -q --qf '%{ARCH}' hyperhdr)
		pass pkg.installed "hyperhdr $PKG_VER ($PKG_ARCH, rpm)"
	elif [ -n "$BIN" ]; then
		warn pkg.installed "no hyperhdr package, but $BIN exists" "a source install; package checks are skipped"
	else
		warn pkg.installed "HyperHDR is not installed" "install the package for this system (dpkg/apt, rpm or pacman)"
	fi

	APP_VER=""
	if [ -z "$BIN" ]; then
		skip bin.version "no hyperhdr binary on PATH"
		skip bin.libraries "no hyperhdr binary on PATH"
	else
		# --version exits before the daemon starts (tests/cli).
		VOUT=$(ev bin.version "$BIN" --version)
		APP_VER=$(printf '%s\n' "$VOUT" | sed -n 's/.*Version *: *\([^ ]*\).*/\1/p' | head -1)
		if [ -n "$APP_VER" ]; then pass bin.version "$APP_VER"
		else fail bin.version "hyperhdr --version printed no version" "see evidence/bin.version.txt; often a missing library (bin.libraries)"; fi
		REAL=$(readlink -f "$BIN")
		have sha256sum && sha256sum "$REAL" >"$EV/bin.sha256.txt" 2>&1
		if have ldd; then
			MISSING=$(ev bin.libraries ldd "$REAL" | awk '/not found/ {print $1}' | sort -u | tr '\n' ' ' | sed 's/ $//')
			if [ -n "$MISSING" ]; then fail bin.libraries "missing: $MISSING" "install the packages that provide these libraries, or reinstall the HyperHDR package"
			else pass bin.libraries "all shared libraries resolve"; fi
		else skip bin.libraries "ldd not installed"; fi
	fi

	if [ -n "$PKG_VER" ] && [ -n "$APP_VER" ]; then
		BASE=${APP_VER%%-*}; PRE=""; [ "$APP_VER" != "$BASE" ] && PRE=${APP_VER#*-}
		squash() { printf '%s' "$1" | tr -d '.~+-'; }
		if [ "${PKG_VER#"$BASE"}" = "$PKG_VER" ]; then
			warn pkg.version "package $PKG_VER, application $APP_VER" "package and binary disagree; reinstall the package"
		elif [ -z "$PRE" ]; then
			pass pkg.version "package $PKG_VER matches application $APP_VER"
		elif [[ "$PKG_VER" == *"~"* ]] && [[ "$(squash "$PKG_VER")" == *"$(squash "$PRE")"* ]]; then
			pass pkg.version "package $PKG_VER matches prerelease $APP_VER"
		else
			warn pkg.version "package declares $PKG_VER for prerelease $APP_VER" "built before the #20 packaging fix; the next corrected package needs 'apt install --allow-downgrades' once"
		fi
	else
		skip pkg.version "needs both an installed package and a working binary"
	fi
fi

# --- runtime -------------------------------------------------------------------
if want runtime; then
	GROUP=runtime
	UNIT="hyperhdr@$SVC_USER.service"
	RUNNING=0
	if have systemctl && [ -d "$R/run/systemd/system" ]; then
		ACTIVE=$(systemctl is-active "$UNIT" 2>/dev/null); ENABLED=$(systemctl is-enabled "$UNIT" 2>/dev/null)
		systemctl status --no-pager "$UNIT" >"$EV/svc.status.txt" 2>&1
		case "$ACTIVE" in
			active) RUNNING=1; pass svc.active "$UNIT is active (enabled: ${ENABLED:-unknown})";;
			failed) fail svc.active "$UNIT has failed" "journalctl -u $UNIT -n 50; the log checks below summarise recent errors";;
			*) if [ "$ENABLED" = enabled ]; then warn svc.active "$UNIT is ${ACTIVE:-inactive} but enabled" "sudo systemctl start $UNIT"
			   else warn svc.active "$UNIT is ${ACTIVE:-not installed}" "sudo systemctl enable --now $UNIT"; fi;;
		esac
		NRESTARTS=$(systemctl show -p NRestarts --value "$UNIT" 2>/dev/null)
		if [ -n "$NRESTARTS" ] && [ "$NRESTARTS" -gt 0 ] 2>/dev/null; then
			warn svc.restarts "$UNIT restarted $NRESTARTS time(s)" "it is crashing or exiting; check the log checks and journalctl -u $UNIT"
		elif [ -n "$NRESTARTS" ]; then pass svc.restarts "no automatic restarts"; fi
	else
		skip svc.active "systemd not available"
	fi

	if ! have pgrep; then PROCS=-1; skip proc.count "pgrep not installed" "install procps"
	else PROCS=$(pgrep -x hyperhdr 2>/dev/null | wc -l); fi
	if [ "$PROCS" -lt 0 ]; then :
	elif [ "$PROCS" -gt 1 ]; then fail proc.count "$PROCS hyperhdr processes" "only one instance may run; stop the extra one (ps -C hyperhdr -o pid,user,args)"
	elif [ "$PROCS" -eq 1 ]; then RUNNING=1; pass proc.count "one hyperhdr process ($(ps -o user= -C hyperhdr 2>/dev/null | head -1 | tr -d ' '))"
	else warn proc.count "hyperhdr is not running" "start the service, or run hyperhdr for the user"; fi

	if [ ! -d "$USERDATA" ]; then
		warn data.dir "$USERDATA does not exist" "it is created on first start of hyperhdr as $SVC_USER"
	else
		OWNER=$(stat -c %U "$USERDATA" 2>/dev/null)
		if [ "$OWNER" != "$SVC_USER" ]; then
			fail data.dir "$USERDATA is owned by $OWNER, service runs as $SVC_USER" "sudo chown -R $SVC_USER: $USERDATA"
		elif [ "$(stat -c %A "$USERDATA" | cut -c3)" != w ]; then
			fail data.dir "$USERDATA is not writable by its owner" "chmod u+w $USERDATA"
		else pass data.dir "$USERDATA owned by $SVC_USER"; fi
		DB="$USERDATA/db/hyperhdr.db"
		if [ ! -f "$DB" ]; then
			warn data.db "$DB missing" "created on first start; if HyperHDR has run, check data.dir ownership"
		elif [ "$(stat -c %U "$DB")" != "$SVC_USER" ]; then
			fail data.db "$DB owned by $(stat -c %U "$DB")" "sudo chown $SVC_USER: $DB"
		elif [ "$DBCHECK" -eq 1 ]; then
			if have sqlite3; then
				QC=$(sqlite3 -readonly "$DB" 'PRAGMA quick_check;' 2>&1 | head -3)
				if [ "$QC" = ok ]; then pass data.db "$DB passes PRAGMA quick_check (read-only)"
				else fail data.db "quick_check: $QC" "restore the backup of $USERDATA, or start over with hyperhdr --deleteDatabase"; fi
			else skip data.db "sqlite3 not installed for --db-check"; fi
		else pass data.db "$DB present ($(stat -c %s "$DB") bytes; contents not read)"; fi
	fi

	LUT=""
	for f in "$USERDATA/lut_lin_tables.3d" "$R/usr/share/hyperhdr/lut/lut_lin_tables.3d"; do [ -f "$f" ] && { LUT=$f; break; }; done
	if [ -n "$LUT" ]; then pass data.lut "$LUT ($(( $(stat -c %s "$LUT") / 1048576 )) MB)"
	else warn data.lut "no lut_lin_tables.3d in $USERDATA or /usr/share/hyperhdr/lut" "needed for HDR tone mapping; create it with the LUT calibration wizard in the web UI, or copy one into $USERDATA"; fi

	if have ss; then
		ss -H -ltnup 2>/dev/null >"$EV/net.listen.txt"
		for spec in 8090/tcp/web 8092/tcp/https 19444/tcp/json 19400/tcp/flatbuffers; do
			port=${spec%%/*}; rest=${spec#*/}; proto=${rest%%/*}; name=${rest#*/}
			LINE=$(awk -v p=":$port" -v t="$proto" '$1==t && $5 ~ p"$"' "$EV/net.listen.txt" | head -1)
			if [ -z "$LINE" ]; then
				if [ "$RUNNING" -eq 1 ]; then warn "net.port.$port" "$name port $port not listening" "disabled in settings, or HyperHDR failed to bind it (see log.errors)"
				else skip "net.port.$port" "$name port $port not listening (HyperHDR not running)"; fi
			else
				case "$LINE" in
					*'"hyperhdr"'*) pass "net.port.$port" "$name port $port held by hyperhdr";;
					*users:*) fail "net.port.$port" "$name port $port held by $(printf '%s' "$LINE" | sed -n 's/.*users:(("\([^"]*\)".*/\1/p')" "stop that program or change the port in HyperHDR settings";;
					*) pass "net.port.$port" "$name port $port listening (owner not visible without root)";;
				esac
			fi
		done
	else skip net.ports "ss not installed" "install iproute2"; fi

	if [ "$RUNNING" -eq 1 ] && have curl; then
		CODE=$(curl -s -o /dev/null -m 5 -w '%{http_code}' http://127.0.0.1:8090/ 2>/dev/null)
		case "$CODE" in 200|301|302) pass net.webui "web UI answers on http://127.0.0.1:8090 ($CODE)";;
			*) warn net.webui "web UI on 127.0.0.1:8090 returned ${CODE:-nothing}" "check net.port.8090 and the webConfig port setting";; esac
	elif [ "$RUNNING" -eq 1 ]; then skip net.webui "curl not installed"
	else skip net.webui "HyperHDR not running"; fi

	if have journalctl; then
		journalctl --no-pager -o cat --since "-$SINCE" -u "$UNIT" 2>/dev/null | sed -E 's/\x1b\[[0-9;]*m//g' >"$EV/log.journal.txt"
		ERRS=$(grep -c '<ERROR>' "$EV/log.journal.txt"); WARNS=$(grep -c '<WARNING>' "$EV/log.journal.txt")
		FIRST=$(grep -m1 -E '<ERROR>' "$EV/log.journal.txt" | cut -c1-160)
		if [ ! -s "$EV/log.journal.txt" ]; then skip log.errors "no journal entries for $UNIT since -$SINCE"
		elif [ "$ERRS" -gt 0 ]; then warn log.errors "$ERRS error(s), $WARNS warning(s) since -$SINCE; first: $FIRST" "see evidence/log.journal.txt"
		elif [ "$WARNS" -gt 0 ]; then warn log.errors "$WARNS warning(s) since -$SINCE" "see evidence/log.journal.txt"
		else pass log.errors "no errors or warnings since -$SINCE"; fi
		# V4L2 capture categories from the grabber's recovery logic.
		for cat in "no signal:|the receiver reports no source; connect or power on the HDMI source" \
		           "timeout:|frames stopped while the receiver reported no loss; check the source and cable" \
		           "initialization failed:|discovery, format negotiation or buffer setup failed; check capture.* checks" \
		           "streaming failed:|the driver reported a streaming error; see the kernel log (journalctl -k)"; do
			key=${cat%%|*}; hint=${cat#*|}
			N=$(grep -c -- "$key" "$EV/log.journal.txt")
			[ "$N" -gt 0 ] && warn "log.capture.$(printf '%s' "${key%%:*}" | tr ' ' '_')" "$N '$key' line(s): $(grep -m1 -- "$key" "$EV/log.journal.txt" | sed "s/.*\($key\)/\1/" | cut -c1-120)" "$hint"
		done
	else skip log.errors "journalctl not available"; fi
fi

# --- capture and LED output ------------------------------------------------------
if want capture; then
	GROUP=capture
	UGROUPS=" $(id -nG "$SVC_USER" 2>/dev/null) "
	NODES=$(ls "$R"/dev/video* 2>/dev/null | sort -V)
	if [ -z "$NODES" ]; then
		skip grp.video "no /dev/video* nodes" "no capture device is connected or its driver is not loaded"
	elif [[ "$UGROUPS" == *" video "* ]]; then pass grp.video "$SVC_USER is in group video"
	else fail grp.video "$SVC_USER is not in group video" "sudo usermod -aG video $SVC_USER, then restart the service"; fi

	SERIAL=$(ls "$R"/dev/ttyUSB* "$R"/dev/ttyACM* 2>/dev/null | tr '\n' ' ')
	if [ -n "$SERIAL" ]; then
		if [[ "$UGROUPS" == *" dialout "* ]]; then pass grp.dialout "$SVC_USER is in group dialout (serial: $SERIAL)"
		else warn grp.dialout "$SVC_USER is not in group dialout (serial: $SERIAL)" "needed for serial LED controllers: sudo usermod -aG dialout $SVC_USER"; fi
	fi
	for kind in spidev hidraw; do
		DEVS=$(ls "$R"/dev/$kind* 2>/dev/null)
		[ -z "$DEVS" ] && continue
		GRP=$(stat -c %G $(printf '%s\n' "$DEVS" | head -1) 2>/dev/null)
		if [[ "$UGROUPS" == *" $GRP "* ]] || [ "$GRP" = "$SVC_USER" ]; then pass "dev.$kind" "$(echo $DEVS | wc -w) $kind device(s), group $GRP accessible"
		else warn "dev.$kind" "$kind devices belong to group $GRP" "needed only for $kind LED output: sudo usermod -aG $GRP $SVC_USER"; fi
	done

	if [ -n "$NODES" ]; then
		if ! have v4l2-ctl; then
			skip capture.nodes "v4l2-ctl not installed" "install v4l-utils for capture checks"
		else
			CAPS=0; RX=""
			for n in $NODES; do
				INFO=$(ev capture.nodes v4l2-ctl -d "${n#$R}" --info)
				DRV=$(printf '%s\n' "$INFO" | sed -n 's/.*Driver name *: *//p' | head -1)
				BUS=$(printf '%s\n' "$INFO" | sed -n 's/.*Bus info *: *//p' | head -1)
				# Device Caps describe this node; Capabilities describe the whole device.
				DCAPS=$(printf '%s\n' "$INFO" | awk '/Device Caps/ {f=1; next} f && /^\t\t/ {print; next} f {f=0}')
				[ -z "$DCAPS" ] && DCAPS=$(printf '%s\n' "$INFO" | awk '/^\t\t/')
				if printf '%s\n' "$DCAPS" | grep -qE '^[[:space:]]*Video Capture( Multiplanar)?[[:space:]]*$'; then
					CAPS=$((CAPS + 1))
					API=$(printf '%s\n' "$DCAPS" | grep -q 'Video Capture Multiplanar' && echo MPLANE || echo single-planar)
					pass "capture.node.$(basename "$n")" "$DRV ($API) identity v4l2:$DRV@$BUS"
					case "$DRV" in *hdmirx*) RX=${n#$R};; esac
				fi
			done
			if [ "$CAPS" -eq 0 ]; then warn capture.nodes "no capture-capable node among $(echo $NODES | wc -w)" "only codec/scaler/metadata nodes are present; connect a grabber or load the receiver driver"
			elif [ "$CAPS" -gt 1 ]; then warn capture.autoselect "$CAPS capture devices" "with 'auto', HyperHDR picks by name order (#11); select and save the device you want in Video settings"
			else pass capture.autoselect "one capture device"; fi

			if [ -n "$RX" ]; then
				CTRLS=$(ev capture.receiver v4l2-ctl -d "$RX" --list-ctrls-menus)
				PP=$(printf '%s\n' "$CTRLS" | sed -n 's/.*power_present.*value=\([^ ]*\).*/\1/p' | head -1)
				DV=$(ev capture.receiver v4l2-ctl -d "$RX" --query-dv-timings)
				FMT=$(ev capture.receiver v4l2-ctl -d "$RX" --get-fmt-video)
				if [ -n "$PP" ] && [ $((PP)) -eq 0 ] 2>/dev/null; then
					warn capture.signal "$RX: no source detected (power_present=0)" "connect and power on the HDMI source; HyperHDR waits and starts automatically"
				elif printf '%s\n' "$DV" | grep -qiE 'severed|ENOLINK|no link|not locked|ENOLCK|out of range|ERANGE'; then
					warn capture.signal "$RX: $(printf '%s\n' "$DV" | grep -m1 -iE 'failed' | sed 's/.*failed: *//')" "check the source output and cable"
				elif printf '%s\n' "$DV" | grep -q 'Active width'; then
					pass capture.signal "$RX: $(printf '%s\n' "$DV" | sed -n 's/.*Active width: *//p')x$(printf '%s\n' "$DV" | sed -n 's/.*Active height: *//p') detected"
				else skip capture.signal "$RX: signal state not reported"; fi
				Q=$(printf '%s\n' "$FMT" | sed -n 's/.*Quantization *: *//p' | head -1)
				RR=$(printf '%s\n' "$CTRLS" | sed -n 's/.*rgb_quantization_range.*value=[0-9]* (\([^)]*\)).*/\1/p' | head -1)
				pass capture.range "format quantization ${Q:-unknown}, receiver RGB range ${RR:-not exposed}"
				if have fuser; then
					HOLD=$(fuser "$RX" 2>/dev/null | tr -s ' ')
					if [ -n "$HOLD" ]; then pass capture.holder "$RX held by pid(s)$HOLD"; else pass capture.holder "$RX not held by a visible process"; fi
				else skip capture.holder "fuser not installed" "install psmisc"; fi
			fi
		fi
	fi

	if [ -n "$LED_HOST" ]; then
		if have ping && ping -c 1 -W 2 "$LED_HOST" >/dev/null 2>&1; then pass led.ping "$LED_HOST answers ping"
		else warn led.ping "$LED_HOST does not answer ping" "check the address; some controllers drop ICMP"; fi
		if have curl; then
			INFO=$(curl -s -m 5 "http://$LED_HOST/json/info" 2>/dev/null)
			VER=$(printf '%s' "$INFO" | sed -n 's/.*"ver" *: *"\([^"]*\)".*/\1/p')
			if [ -n "$VER" ]; then pass led.wled "WLED $VER on $LED_HOST"
			else warn led.wled "no WLED JSON API on $LED_HOST" "not a WLED controller, or unreachable over HTTP"; fi
		else skip led.wled "curl not installed"; fi
	fi
fi

# --- report --------------------------------------------------------------------
NFAIL=0; NWARN=0; NPASS=0; NSKIP=0
for s in "${STATS[@]}"; do case $s in FAIL) NFAIL=$((NFAIL+1));; WARN) NWARN=$((NWARN+1));; PASS) NPASS=$((NPASS+1));; SKIP) NSKIP=$((NSKIP+1));; esac; done

json_str() {
	local s=$1
	s=${s//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\t'/\\t}; s=${s//$'\n'/\\n}; s=${s//$'\r'/}
	printf '"%s"' "$(printf '%s' "$s" | tr -d '\000-\010\013\014\016-\037')"
}
emit_json() {
	printf '{"schema":1,"tool":"hyperhdr-doctor","user":%s,"user_source":%s,"userdata":%s,"summary":{"pass":%d,"warn":%d,"fail":%d,"skip":%d},"checks":[' \
		"$(json_str "$SVC_USER")" "$(json_str "$SVC_SOURCE")" "$(json_str "$USERDATA")" $NPASS $NWARN $NFAIL $NSKIP
	local i
	for i in "${!IDS[@]}"; do
		[ "$i" -gt 0 ] && printf ','
		local evf=""; [ -f "$EV/${IDS[$i]}.txt" ] && evf="evidence/${IDS[$i]}.txt"
		printf '{"id":%s,"group":%s,"status":%s,"finding":%s,"hint":%s,"evidence":%s}' \
			"$(json_str "${IDS[$i]}")" "$(json_str "${GRPS[$i]}")" "$(json_str "${STATS[$i]}")" \
			"$(json_str "${FINDS[$i]}")" "$(json_str "${HINTS[$i]}")" "$(json_str "$evf")"
	done
	printf ']}\n'
}

REDACT_HOST=$(hostname 2>/dev/null || true)
REDACT_USER=$SVC_USER
if [ "$JSON" -eq 1 ]; then
	emit_json | REDACT_HOST="$REDACT_HOST" REDACT_USER="$REDACT_USER" hh_redact
else
	echo
	echo "Summary: $NPASS pass, $NWARN warn, $NFAIL fail, $NSKIP skip (service user $SVC_USER from $SVC_SOURCE)"
fi

if [ -n "$ARCHIVE" ]; then
	STAMP=$(date -u +%Y%m%dT%H%M%SZ); B="$WORK/hyperhdr-doctor-$STAMP"; mkdir -p "$B/evidence"
	emit_json >"$B/report.json"
	for i in "${!IDS[@]}"; do printf '[%s] %s: %s%s\n' "${STATS[$i]}" "${IDS[$i]}" "${FINDS[$i]}" "${HINTS[$i]:+ -> ${HINTS[$i]}}"; done >"$B/report.txt"
	for f in "$EV"/*.txt; do [ -f "$f" ] && cp "$f" "$B/evidence/"; done
	for f in "$B"/report.* "$B"/evidence/*; do
		[ -f "$f" ] || continue
		REDACT_HOST="$REDACT_HOST" REDACT_USER="$REDACT_USER" hh_redact <"$f" >"$f.r" && mv "$f.r" "$f"
	done
	mkdir -p "$ARCHIVE"
	tar -C "$WORK" -czf "$ARCHIVE/hyperhdr-doctor-$STAMP.tar.gz" "hyperhdr-doctor-$STAMP"
	[ "$JSON" -eq 1 ] || echo "Archive: $(cd "$ARCHIVE" && pwd)/hyperhdr-doctor-$STAMP.tar.gz (redacted; review before sharing)"
fi

[ "$NFAIL" -gt 0 ] && exit 1
exit 0
