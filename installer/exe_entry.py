"""Frozen (.exe) entry point for MTF-Recomp-Setup.exe (PyInstaller)."""
import sys

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--cli':
        from installer.cli import main as cli_main
        sys.exit(cli_main(sys.argv[2:]))
    else:
        from installer.gui import main as gui_main
        sys.exit(gui_main(sys.argv[1:]))
