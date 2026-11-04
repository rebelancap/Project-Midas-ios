# Contributing to Project Midas

Project Midas is a PC port of GE built with care: matching the original game where it matters, with every change tested in game. Contributions are welcome when they meet that bar. Please contribute here rather than starting a fork, so the work stays in one place.

## Already in progress

These are being worked on now. If you want to help with one, open an issue first so effort isn't duplicated.

| Area | Status |
| --- | --- |
| Co-op | In development |
| Netplay | In development |
| Mod loading (xdelta patches) | In development |
| Lua modding | Planned |

## Small changes: Lua mods

Once Lua modding is developed and released, minor changes (tweaks to weapons, rules, HUD or other gameplay details) should be made as Lua mods rather than pull requests. That keeps the port itself stable and lets players choose the changes they want. Until then, open an issue to discuss a small change before sending a pull request.

## What a good pull request looks like

1. **One focused change.** Fix one bug or add one feature per PR. Don't mix in unrelated cleanup.
2. **Builds on both platforms.** CI builds Linux and Windows; it must pass.
3. **Tested in game.** Say what you tested and on which level or menu. "It compiles" is not enough.
4. **Matches the code around it.** Keep the decompiled names. Keep comments short: what the code does and why, in at most two plain sentences.
5. **No game assets.** Never commit ROM data or extracted assets.

## AI-assisted contributions

AI tools are fine to use for code. Low-effort AI output is not.

- **Code only.** AI-generated images, textures, models, sound or music will never be accepted. Art and audio must be made by people.
- Say in the PR that AI was used (the PR template has a box for it).
- You must understand every line you submit and be able to answer questions about it.
- You must have run and tested the change yourself.
- Large unreviewed or untested AI changes will be closed without review.

## Forks

The code is MIT licensed, so you are free to fork it. If you do, please:

- give your fork its own name, not "Project Midas",
- link back to this repository,
- send fixes back here as pull requests so everyone benefits.

## License

By contributing you agree that your contribution is licensed under the MIT license in [LICENSE](LICENSE). That license covers the port code only; see its "Scope" section.
