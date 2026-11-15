# Shared redaction for diagnostic output (sourced, not executed).
#
#   REDACT_HOST=<hostname> REDACT_USER=<name> hh_redact < in > out
#
# Replaces IPv4, IPv6 and MAC addresses, the hostname and the user name.
# Timestamps (hh:mm:ss), bus info (platform:fdee0000.hdmi_receiver) and
# version strings are kept. "root" is not treated as identifying.
hh_redact() {
	local host=${REDACT_HOST:-} user=${REDACT_USER:-}
	[ "$user" = root ] && user=""
	sed -E \
		-e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ipv4>/g' \
		-e 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/<mac>/g' \
		-e 's/\b([0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}\b/<ipv6>/g' \
		-e 's/\b[0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*::([0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*)?\b/<ipv6>/g' \
		${host:+-e "s/\\b${host}\\b/<host>/g"} \
		${user:+-e "s/\\b${user}\\b/<user>/g"}
}
