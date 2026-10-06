# FST of the test instance.
xrd.network keepalive
xrd.port 1095
xrootd.fslib -2 libXrdEosFst.so
xrootd.async off nosf
xrootd.redirect eosmirror-mgm.eosmirror.test:1094 chksum
xrootd.seclib libXrdSec.so
sec.protocol unix
sec.protocol sss -c /etc/eos.keytab -s /etc/eos.keytab
sec.protbind * only unix sss
all.export / nolock
all.trace none
all.manager localhost 2131
ofs.persist off
ofs.osslib libEosFstOss.so
ofs.tpc pgm /opt/eos/xrootd/bin/xrdcp
fstofs.autoboot true
fstofs.quotainterval 10
fstofs.metalog /var/eos/md/
fstofs.filemd_handler attr
fstofs.qdbcluster eosmirror-qdb.eosmirror.test:7777
fstofs.qdbpassword_file /etc/eos/qdb.password
