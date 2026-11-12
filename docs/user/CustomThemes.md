# Custom Themes

The interface can be restyled with your own themes, without rebuilding the emulator.

## Installing a theme

1. Open **Emulation > Configure > General > UI** and press **Open Themes Folder**.
   This is the `themes` folder of the user folder.
2. Put the theme's folder there. A theme is a folder that holds a `style.qss`:

   ```
   themes/
     art-dark/
       style.qss
       theme.ini      (optional)
       icons/         (optional, whatever the stylesheet uses)
   ```

3. Open the configuration again and pick the theme in the **Theme** list, below the built-in ones.

An example theme is in [`dist/custom_themes/art-dark`](../../dist/custom_themes/art-dark).

## Writing a theme

`style.qss` is a [Qt style sheet](https://doc.qt.io/qt-6/stylesheet-reference.html), close to CSS.
Saving it, or `theme.ini`, updates the running emulator: no restart.

Files of the theme's folder are reached with the `theme:` prefix:

```css
QCheckBox::indicator:checked {
    image: url("theme:icons/check.svg");
}
```

### theme.ini

Every key is optional.

```ini
[Theme]
name=Art Dark
base=lichii
dark=true
inherit=false
accent=#82a88b
```

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | the folder's name | The name in the theme list. |
| `base` | `lichii` | The built-in theme that provides the icons and the palette: `default`, `colorful`, `qdarkstyle`, `colorful_dark`, `qdarkstyle_midnight_blue`, `colorful_midnight_blue` or `lichii`. |
| `dark` | whether `base` is dark | Dark title bar and link colours. |
| `accent` | the base theme's | The colour of the selected category and tab icons, of the favourite star, of the selection and of links, as `#rrggbb`. |
| `inherit` | `false` | `true` applies the base theme's stylesheet first, then `style.qss`. Use it for a theme that changes a few things of a built-in one. |

### Performance overlay

The overlay paints itself, so its colours are properties of `PerformanceOverlay`:

```css
PerformanceOverlay {
    qproperty-backgroundColor: rgba(13, 16, 14, 140);
    qproperty-borderColor: #303531;
    qproperty-cornerRadius: 10;
    qproperty-accentColor: #82a88b;
}
```

| Property | Used for |
| --- | --- |
| `backgroundColor` | The panel. |
| `borderColor` | The panel's outline; none by default. |
| `cornerRadius` | The panel's rounded corners, in pixels. |
| `textColor` | Values. |
| `mutedColor` | Units, tags and secondary values. |
| `dimColor` | The faintest labels. |
| `accentColor` | The limiting load. |
| `lineColor` | The frame time graph's line. |
| `spikeColor` | Frame time spikes. |
| `axisColor` | Axes and grid lines. |
| `medianLineColor` | The frame time graph's median line. |

A property the stylesheet doesn't set keeps its built-in colour.
