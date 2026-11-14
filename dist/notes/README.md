# Adventure notes modules

A module adds a game to the Adventure notes panel. The emulator knows no save format: the module
reads the save and returns what to show. Adding a game never needs a rebuild.

Open the panel in a game and press "Create a module for this game": it writes
`notes/<PROGRAM ID>.js` in the user folder and opens the folder. Edit the file: the panel reloads
it when you save, and shows any error with its line. If the save file's name is wrong, the panel
lists the files the game saves. A `notes` folder next to the program is read when the user has none.

## A module

```js
var game = { title: "My game", folder: "0100000000010000", file: "progress.sav" };

function read(save) {
    const view = new DataView(save.data);
    return {
        title: "Shown large at the top",
        tabs: [{ name: "Stats", views: [{ name: "Summary", rows: [
            { title: "Coins", value: view.getUint32(0x10, true) },
            { title: "Boss beaten", check: true },
            { title: "Stars", done: 12, total: 120 },
        ] }] }],
    };
}
```

`accent` in `game` is the panel's colour. `keys` in the result, a few `{ title, value }`, stay in
view at the top of the wide layout. `folder` is the game's folder in the save directory. `file` is looked for there and in each slot
folder, and the newest copy is used. `read` runs when the save changes. A tab with several views
shows a list to pick one. A row has a `value`, a tick (`check`) or a bar (`done`, `total`).

## What a module can use

- `save.data`: the file as an ArrayBuffer. `save.slot`: the name of its folder.
- `notes.file(name)`: another file of the same save, as an ArrayBuffer.
- `notes.unzip(data, offset, end)`: inflates a zlib stream.
- `notes.utf8(data, offset, size)`: text, up to the first zero.
- `notes.include(name)`: runs a shared library from `notes/lib`, like `gamedata-table.js`.

## Following the running game

A module may also define `tick()`. Lichii calls it ten times a second while the game runs and is
not paused; returning `true` asks the panel to call `read` again and redraw.

- `notes.memory(address, size)`: up to 4 KiB of the running game's memory (address as a hex
  string), as an ArrayBuffer, or null when a page is not mapped. Read only.
- `notes.search(pattern)`: looks for a byte pattern (hex, up to 256 bytes) in the game's heap on a
  worker thread. It returns null while the search runs; call it again with the same pattern until
  it returns the addresses found (hex strings, at most 64).
- `notes.load(name)`, `notes.store(name, text)`: a small text the module keeps between sessions,
  in `notes/state`.

The Ocarina of Time module uses them for its Timers tab: it finds the record where the game keeps
its zone and room, then times every place, dungeon and boss room to a tenth of a second.

A module reaches nothing else: no other file, no network, no writes to the save or the game's
memory. It is stopped after 5 seconds. Throwing
an Error shows its message in the panel, which suits a save version the module does not know.

Prefer typed arrays and plain objects in long loops: `Map` and `Set` search their keys one by one
in this engine, and `DataView` calls are slow. Keep game content out of a module: identifiers and
short names only.

The Pokopia module's layout comes from [PokoHex](https://github.com/berichan/PokoHex) (GPL-3.0).
