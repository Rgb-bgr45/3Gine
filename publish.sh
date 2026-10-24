#!/data/data/com.termux/files/usr/bin/sh
# Copies the project saved by the 3gine editor into this repo and pushes it.
# GitHub Actions then builds the game APK. Run from the repo root:  sh publish.sh
SRC="$HOME/storage/shared/Documents/3gine/game"
if [ ! -d "$SRC" ]; then
  echo "Not found: $SRC"
  echo "Run 'termux-setup-storage' once, and press Save in the 3gine editor."
  exit 1
fi
mkdir -p game
cp -r "$SRC"/. game/
git add game
git commit -m "game update" || echo "nothing new to commit"
git push
