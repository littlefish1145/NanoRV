# Native networking runtime

RVVM dynamically loads libslirp; these DLLs can be replaced with compatible
builds without recompiling RVVM. No QEMU executable is used by this backend.
Copyright/license texts are retained in `LICENSES/`.

| Component | Origin | Version |
|---|---|---|
| libslirp-0.dll, headers | MSYS2 UCRT64 package | 4.9.3-1 |
| libglib-2.0-0.dll | supplied xPack QEMU runtime | 2.86.0 |
| libintl-8.dll | supplied xPack QEMU runtime | gettext 0.26 |
| libpcre2-8-0.dll | supplied xPack QEMU runtime | 10.46 |

Package: https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-libslirp-4.9.3-1-any.pkg.tar.zst

Package SHA-256: `c982be43767a950f220b498f0f212eb2e2c945d93c968bd0e4f66b1b5da94f6c`.
Its original metadata and headers are under `../libslirp/`.

Runtime SHA-256:

```
4b547f2324ddba5bf5222ea31530b621e8ca7baf4884c5c52059aa88b64af782 libslirp-0.dll
ae69377d80f07059019d468bdacdcebe76d54911548e40f41300e3574cfc3a8e libglib-2.0-0.dll
468251c1ef17de22b2a810136e4244b738535e78b2b720e19716690932e49e8e libintl-8.dll
c18f8953bab605071151274b95b3cd1295401fd4c38bfdcea6e5e3ca7860c9ad libpcre2-8-0.dll
```

Sources: https://gitlab.freedesktop.org/slirp/libslirp (QEMU mirror:
https://github.com/qemu/libslirp), https://gitlab.gnome.org/GNOME/glib,
https://www.gnu.org/software/gettext/, https://github.com/PCRE2Project/pcre2.
The xPack dependency build records/licenses are also retained in the supplied
`xpack-qemu-riscv-9.2.4-1/distro-info/` directory.
