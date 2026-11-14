// Saves made of hash and value pairs from 0x28, up to the offset held at 8, then 64-bit keys.
// Ocarina of Time uses it. gamedataReport(save, data) turns a description of what to show into
// the panel's tabs: see 0400486024DEC000.js.

function gamedataRead(save, data) {
    const view = new DataView(save.data);
    const size = save.data.byteLength;
    if (size < 0x30) {
        throw new Error("This save is too short");
    }
    // Typed arrays: these loops see every word of the save, and DataView calls are much slower.
    const words = new Uint32Array(save.data, 0, size >> 2);
    const end = words[2];
    // Plain objects: Map looks its keys up one by one in this engine, which takes most of a second.
    const table = {};
    for (let word = 10; word * 4 + 8 <= size && word * 4 < end; word += 2) {
        if (table[words[word]] === undefined) {
            table[words[word]] = words[word + 1];
        }
    }
    const split = (key) => {
        const text = key.replace("0x", "").padStart(16, "0");
        return [parseInt(text.slice(8), 16), parseInt(text.slice(0, 8), 16)];
    };
    // Only the keys a module asks for are kept: saves hold many more.
    const wanted = {};
    // data.extra_keys: keys a module checks itself, outside its collections.
    const keys = (data.extra_keys || []).slice();
    for (const collection of data.collections) {
        for (const zone of collection.zones) {
            keys.push(...zone.keys);
        }
    }
    for (const key of keys) {
        const [low, high] = split(key);
        wanted[low] = wanted[low] || {};
        wanted[low][high] = false;
    }
    for (let word = (end - (end % 8)) >> 2; word * 4 + 8 <= size; word += 2) {
        const highs = wanted[words[word]];
        if (highs !== undefined && highs[words[word + 1]] === false) {
            highs[words[word + 1]] = true;
        }
    }
    return {
        read(hash, kind) {
            const value = table[parseInt(hash, 16)];
            if (value === undefined) {
                return 0;
            }
            if (kind === "flag" || kind === "number") {
                return value;
            }
            if (value + 8 > size) {
                return 0;
            }
            return view.getUint32(value, true) + view.getInt32(value + 4, true) * 4294967296;
        },
        count(keys) {
            return keys.filter((key) => {
                const [low, high] = split(key);
                return wanted[low] !== undefined && wanted[low][high] === true;
            }).length;
        },
    };
}

// A zone belongs to a view of the same name, or to it with a suffix such as " (child)".
function sameZone(zone, view) {
    return zone.name === view.name || zone.name.startsWith(view.name + " (");
}

function gamedataReport(save, data) {
    const table = gamedataRead(save, data);
    const zoneTotal = (collection, measure) =>
        collection.zones.reduce((sum, zone) => sum + measure(zone), 0);
    const found = (collection) => zoneTotal(collection, (zone) => table.count(zone.keys));
    const size = (collection) => zoneTotal(collection, (zone) => zone.keys.length);

    const global = [];
    for (const view of data.views) {
        const flags = (view.fields || []).filter((field) => field.kind === "flag");
        if (view.checklist && flags.length > 0) {
            // A view's score also counts the collection zones of the same name
            // (a dungeon's Gold Skulltulas and its boss's Heart Container).
            let done = flags.filter((field) => table.read(field.hash, "flag") !== 0).length;
            let total = flags.length;
            for (const collection of data.collections) {
                // "merge": false keeps a collection in its own view only.
                if (collection.merge === false) {
                    continue;
                }
                for (const zone of collection.zones) {
                    if (sameZone(zone, view)) {
                        done += table.count(zone.keys);
                        total += zone.keys.length;
                    }
                }
            }
            global.push({ title: view.name, done: done, total: total });
        }
    }
    for (const collection of data.collections) {
        global.push({ title: collection.name, done: found(collection), total: size(collection) });
    }
    for (const summary of data.summary || []) {
        let value = 0;
        for (const term of summary.terms) {
            const scale = term.scale === undefined ? 1 : term.scale;
            if (term.hash !== undefined) {
                value += table.read(term.hash, "number") * scale;
            } else if (term.collection !== undefined) {
                value += found(data.collections[term.collection]) * scale;
            } else {
                value += term.constant || 0;
            }
        }
        global.push({ title: summary.title, value: value });
    }

    const rowsOf = (view) => {
        if (view.collection !== undefined) {
            return data.collections[view.collection].zones.map((zone) => ({
                title: zone.name,
                done: table.count(zone.keys),
                total: zone.keys.length,
            }));
        }
        const rows = (view.fields || []).map((field) => {
            const value = table.read(field.hash, field.kind);
            return field.kind === "flag"
                ? { title: field.title, check: value !== 0 }
                : { title: field.title, value: value };
        });
        for (const collection of data.collections) {
            if (collection.merge === false) {
                continue;
            }
            for (const zone of collection.zones) {
                if (sameZone(zone, view)) {
                    rows.push({
                        title: collection.name + zone.name.slice(view.name.length),
                        done: table.count(zone.keys),
                        total: zone.keys.length,
                    });
                }
            }
        }
        return rows;
    };

    const tabs = [{ name: "Global", views: [{ name: "Global", rows: global }] }];
    for (const view of data.views) {
        let tab = tabs.find((entry) => entry.name === view.tab);
        if (tab === undefined) {
            tab = { name: view.tab, views: [] };
            tabs.push(tab);
        }
        tab.views.push({ name: view.name, rows: rowsOf(view) });
    }
    const seconds = table.read(data.play_time, "count");
    const minutes = String(Math.floor((seconds % 3600) / 60)).padStart(2, "0");
    return { title: `${Math.floor(seconds / 3600)} h ${minutes}`, tabs: tabs };
}
