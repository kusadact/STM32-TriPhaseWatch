#!/usr/bin/env bash
# 按分支前缀生成下一个版本号；在用户确认合并、且分支已同步最新 master 之后运行。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="$root/VERSION"

branch="${1:-$(git -C "$root" rev-parse --abbrev-ref HEAD)}"

if [ ! -f "$version_file" ]; then
  echo "找不到 $version_file" >&2
  exit 1
fi

current="$(tr -d '[:space:]' < "$version_file")"
IFS='.' read -r major minor patch <<< "$current"

for part in "$major" "$minor" "$patch"; do
  case "$part" in
    ''|*[!0-9]*)
      echo "VERSION 不是 x.y.z 格式：$current" >&2
      exit 1
      ;;
  esac
done

case "$branch" in
  feat/*)
    next="$major.$((minor + 1)).0"
    reason="feat 分支：y 加 1、z 归零"
    ;;
  fix/*)
    next="$major.$minor.$((patch + 1))"
    reason="fix 分支：z 加 1"
    ;;
  *)
    echo "分支 '$branch' 不改变版本号，VERSION 保持 $current"
    exit 0
    ;;
esac

printf '%s\n' "$next" > "$version_file"
echo "$reason"
echo "VERSION: $current → $next"
