# Environment of the FST of the test instance (container environment and
# /etc/sysconfig/eos_env for the start scripts). No quotes: podman reads this
# file literally.
DAEMON_COREFILE_LIMIT=unlimited
LD_PRELOAD=/usr/lib64/libjemalloc.so
EOS_INSTANCE_NAME=eosmirror
EOS_GEOTAG=test::local
EOS_MGM_ALIAS=eosmirror-mgm.eosmirror.test
XRD_ROLES=fst
EOS_FST_NETWORK_INTERFACE=eth0
