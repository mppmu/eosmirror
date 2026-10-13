#!/bin/sh
# Runs the EOS integration test of a sudoer's identity as the sudoer whose
# sss key is in EOSMIRROR_EOS_SUDOER_KEYTAB, which scripts/integration-test.sh
# sets from the test instance. Without that key, the test is skipped (exit
# status 77).
if [ -z "${EOSMIRROR_EOS_URL:-}" ] || [ -z "${EOSMIRROR_EOS_SUDOER_KEYTAB:-}" ]; then
  echo "skipped: needs EOSMIRROR_EOS_URL and EOSMIRROR_EOS_SUDOER_KEYTAB" >&2
  exit 77
fi
XrdSecPROTOCOL=sss XrdSecSSSKT=$EOSMIRROR_EOS_SUDOER_KEYTAB exec "$@" --no-skip \
  --test-case='eos endpoint: a sudoer cannot set owners'
