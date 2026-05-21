#!/bin/bash
# Reads current version from CMakeLists.txt, increments patch, repackages.

CMAKE="$(dirname "$0")/CMakeLists.txt"
CURRENT=$(grep -m1 'project(PatchyPlugin VERSION' "$CMAKE" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
MAJOR=$(echo $CURRENT | cut -d. -f1)
MINOR=$(echo $CURRENT | cut -d. -f2)
PATCH=$(echo $CURRENT | cut -d. -f3)
NEW_PATCH=$((PATCH + 1))
NEW_VER="$MAJOR.$MINOR.$NEW_PATCH"

echo "Bumping $CURRENT → $NEW_VER"

sed -i "s/project(PatchyPlugin VERSION $CURRENT)/project(PatchyPlugin VERSION $NEW_VER)/" "$CMAKE"
sed -i "s/VERSION                  \"$CURRENT\"/VERSION                  \"$NEW_VER\"/" "$CMAKE"
sed -i "s/\"version\": \"$CURRENT\"/\"version\": \"$NEW_VER\"/" "$(dirname "$0")/UI/package.json"

echo "Done — now package as Patchy_v${NEW_VER}.zip"
