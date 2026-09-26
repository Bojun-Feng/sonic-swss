if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd
else
SUBDIRS = fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd 
endif


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif

EXTRA_DIST = lib/intfguard.h doc/vrf-bind-guard.md \
             doc/vrf-bind-guard/diagram-1.mmd doc/vrf-bind-guard/diagram-1.png \
             doc/vrf-bind-guard/diagram-2.mmd doc/vrf-bind-guard/diagram-2.png \
             doc/vrf-bind-guard/diagram-3.mmd doc/vrf-bind-guard/diagram-3.png
