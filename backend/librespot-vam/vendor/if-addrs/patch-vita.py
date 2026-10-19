#!/usr/bin/env python3
"""Apply the VitaSpot patches to the vendored if-addrs crate.

if-addrs only knows Linux/BSD/Apple/Windows; on the Vita it tried to use
Linux netlink and libc::getifaddrs, neither of which exists in newlib. VitaSpot
patches it to build an interface list from SceNetCtl instead.
"""
import pathlib

root = pathlib.Path(__file__).resolve().parent
path = root / "src" / "lib.rs"
text = path.read_text(encoding="utf-8")

text = text.replace(
    "#[cfg(not(windows))]\nmod posix;",
    "#[cfg(all(not(windows), not(target_os = \"vita\")))]\nmod posix;\n#[cfg(all(not(windows), target_os = \"vita\"))]\nmod vita;",
)
text = text.replace(
    '    not(target_os = "illumos")\n))]\nmod posix_not_apple;',
    '    not(target_os = "illumos"),\n    not(target_os = "vita")\n))]\nmod posix_not_apple;',
)
text = text.replace(
    "#[cfg(not(windows))]\nmod getifaddrs_posix {",
    "#[cfg(all(not(windows), not(target_os = \"vita\")))]\nmod getifaddrs_posix {",
)
text = text.replace(
    "#[cfg(not(windows))]\npub fn get_if_addrs() -> io::Result<Vec<Interface>> {\n    getifaddrs_posix::get_if_addrs()\n}",
    "#[cfg(all(not(windows), not(target_os = \"vita\")))]\npub fn get_if_addrs() -> io::Result<Vec<Interface>> {\n    getifaddrs_posix::get_if_addrs()\n}\n\n#[cfg(all(not(windows), target_os = \"vita\"))]\npub fn get_if_addrs() -> io::Result<Vec<Interface>> {\n    vita::get_if_addrs()\n}",
)
text = text.replace(
    '    target_os = "illumos"\n)))]',
    '    target_os = "illumos",\n    target_os = "vita"\n)))]',
)
path.write_text(text, encoding="utf-8")
print("patched", path)
