[//]: # ($FrauBSD: bhotkeys-airplane/CHANGELOG.md 2026-10-05 18:44:10 -0700 Devin Teske $)

# Changelog

Newest first. Each section is a git tag; the bullets are what landed
in that tag (from the previous tag, or from the start of the
repository for 1.0).

## 1.1 (2026-10-05)

- `fn-airplane` and `fn-rfkill` run `airplane-mode toggle`
- drop `airplane-mode-toggle`

## 1.0 (2026-10-05)

- `fn-airplane` (Super+A) and `fn-rfkill` (XF86RFKill) run
  `airplane-mode-toggle`; offered at the greeter
- `airplane-radiosd` serves a socket in group `airplane_radiosd`;
  `airplane-mode` sends `on`, `off`, and `status` there
- man pages for `airplane-mode`, `airplane-mode-toggle`,
  and `airplane-radiosd`
