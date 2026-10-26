# assets

The game's look in the PSP's menu (the XMB). Put any of these files here and `make` builds them into the EBOOT;
without them, the game shows up with the PSP's default icon and no background or sound.

| File | What it is | Format |
| --- | --- | --- |
| `ICON0.png` | The icon in the game list | PNG, 144x80 |
| `PIC1.png` | The background shown while the game is selected | PNG, 480x272 |
| `SND0.at3` | The sound played while the game is selected | ATRAC3, at most about 500 KB |

The names may also be in upper case (`ICON0.PNG`, `PIC1.PNG`, `SND0.AT3`). Run `make` again after adding or
changing one.

They are not in the repository: an icon or a background made from the game's art is SEGA's, so bring your own. Git
ignores everything in this folder but this file.
