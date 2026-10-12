#!/bin/sh
# Runs the integration test of a sudoer's identity on EOS as the sudoer whose
# sss key EOSMIRROR_EOS_SUDOER_KEYTAB holds, which scripts/integration-test.sh
# gives from the test instance. Without one, the test is skipped (exit
# status 77).
if [ -z "${EOSMIRROR_EOS_URL:-}" ] || [ -z "${EOSMIRROR_EOS_SUDOER_KEYTAB:-}" ]; then
  echo "skipped: needs EOSMIRROR_EOS_URL and EOSMIRROR_EOS_SUDOER_KEYTAB" >&2
  exit 77
fi
XrdSecPROTOCOL=sss XrdSecSSSKT=$EOSMIRROR_EOS_SUDOER_KEYTAB exec "$@" --no-skip \
  --test-case='eos endpoint: a sudoer cannot set owners'
