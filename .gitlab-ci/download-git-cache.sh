#!/bin/bash

set +e
set -o xtrace

# if we run this script outside of gitlab-ci for testing, ensure
# we got meaningful variables
CI_PROJECT_DIR=${CI_PROJECT_DIR:-$(mktemp -d)/$CI_PROJECT_NAME}

if [[ -e $CI_PROJECT_DIR/.git ]]
then
    echo "Repository already present, skip cache download"
    exit
fi

TMP_DIR=$(mktemp -d)

echo "Downloading archived master..."
curl --retry 4 -f --retry-all-errors --retry-delay 60 \
	      -o "$TMP_DIR/$CI_PROJECT_NAME.tar.gz" \
              "https://${MINIO_HOST}/git-cache/${FDO_UPSTREAM_REPO}/$CI_PROJECT_NAME.tar.gz"

# check curl error code
if [[ $? -ne 0 ]]
then
    echo "Repository cache not available"
    exit
fi

set -e

rm -rf "$CI_PROJECT_DIR"
echo "Extracting tarball into '$CI_PROJECT_DIR'..."
mkdir -p "$CI_PROJECT_DIR"
tar xzf "$TMP_DIR/$CI_PROJECT_NAME.tar.gz" -C "$CI_PROJECT_DIR"
rm -rf "$TMP_DIR"
chmod a+w "$CI_PROJECT_DIR"
