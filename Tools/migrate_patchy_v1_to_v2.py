#!/usr/bin/env python3
"""
migrate_patchy_v1_to_v2.py — Migrate .patchy files from v1 to v2 format

Changes applied:
  - Renames JSON key "addonName" → "paxName" in all node entries

Usage:
  # Migrate a single file (saves a .bak backup):
  python3 migrate_patchy_v1_to_v2.py patch.patchy

  # Migrate all .patchy files in a folder (recursive):
  python3 migrate_patchy_v1_to_v2.py ~/Documents/Patches/

  # Dry run — shows what would change without writing:
  python3 migrate_patchy_v1_to_v2.py ~/Documents/Patches/ --dry-run

  # Skip backup files:
  python3 migrate_patchy_v1_to_v2.py ~/Documents/Patches/ --no-backup
"""

import argparse
import json
import shutil
import sys
from pathlib import Path


def migrate_node(node: dict) -> bool:
    """Rename addonName → paxName in a single node dict. Returns True if changed."""
    if "addonName" in node and "paxName" not in node:
        node["paxName"] = node.pop("addonName")
        return True
    return False


def migrate_data(data: dict) -> int:
    """Migrate all nodes in a graph dict. Returns number of nodes changed."""
    changed = 0
    for node in data.get("nodes", []):
        if migrate_node(node):
            changed += 1
    return changed


def migrate_file(path: Path, dry_run: bool, backup: bool) -> tuple[int, bool]:
    """
    Migrate a single .patchy file.
    Returns (nodes_changed, file_written).
    """
    try:
        text = path.read_text(encoding="utf-8")
        data = json.loads(text)
    except (OSError, json.JSONDecodeError) as e:
        print(f"  ✗ {path}: {e}", file=sys.stderr)
        return 0, False

    changed = migrate_data(data)

    if changed == 0:
        print(f"  · {path.name}: already up to date")
        return 0, False

    if dry_run:
        print(f"  ~ {path.name}: would rename {changed} node(s) (dry run)")
        return changed, False

    if backup:
        bak = path.with_suffix(".patchy.bak")
        shutil.copy2(path, bak)

    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(f"  ✓ {path.name}: migrated {changed} node(s)"
          + (f" (backup: {path.name}.bak)" if backup else ""))
    return changed, True


def main():
    parser = argparse.ArgumentParser(
        description="Migrate .patchy files from v1 (addonName) to v2 (paxName)"
    )
    parser.add_argument("path", help="File or folder to migrate")
    parser.add_argument("--dry-run",   action="store_true", help="Show changes without writing")
    parser.add_argument("--no-backup", action="store_true", help="Skip .bak backup files")
    args = parser.parse_args()

    target = Path(args.path).expanduser().resolve()
    backup = not args.no_backup

    if not target.exists():
        print(f"Error: {target} does not exist", file=sys.stderr)
        sys.exit(1)

    files = [target] if target.is_file() else sorted(target.rglob("*.patchy"))

    if not files:
        print("No .patchy files found.")
        sys.exit(0)

    print(f"{'Dry run — ' if args.dry_run else ''}Migrating {len(files)} file(s)...\n")

    total_nodes = 0
    total_files = 0

    for f in files:
        if f.suffix != ".patchy":
            continue
        nodes, written = migrate_file(f, dry_run=args.dry_run, backup=backup)
        total_nodes += nodes
        if written:
            total_files += 1

    print(f"\n{'Would migrate' if args.dry_run else 'Migrated'} "
          f"{total_nodes} node(s) across {total_files} file(s).")


if __name__ == "__main__":
    main()
