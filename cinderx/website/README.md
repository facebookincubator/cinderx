# CinderX documentation website

This is the [Docusaurus](https://docusaurus.io/) site that powers the public
CinderX documentation, published to GitHub Pages at
<https://facebookincubator.github.io/cinderx/>.

## Layout

| Path                   | What it holds                                              |
| ---------------------- | --------------------------------------------------------- |
| `docs/`                | The documentation pages, authored as Markdown.            |
| `sidebars.ts`          | The navigation sidebar (which pages appear, and in what order). |
| `docusaurus.config.ts` | Site configuration — title, URLs, navbar, footer, plugins. |
| `src/css/custom.css`   | Theme overrides (the CinderX flame-orange brand color).   |
| `static/`              | Files served verbatim at the site root (logo, `robots.txt`). |

## Writing docs

Documentation pages live in `docs/` and map directly to URLs — e.g.
`docs/Jit/preloading.md` is served at `/jit/preloading`. The Introduction page
(`docs/introduction.md`) is served at the site root (`/`).

To add a page:

1. Create a `.md` file under `docs/`, in the section folder it belongs to.
2. Add front matter at the top of the file:

   ```markdown
   ---
   title: Page Title
   description: One-sentence summary, used for SEO and link previews.
   slug: /custom/url        # optional; omit to use the file path
   sidebar_label: Short Label   # optional; defaults to the title
   ---
   ```

3. Register the page in `sidebars.ts` so it shows up in the navigation. The id
   is the file path under `docs/` without the extension (e.g.
   `Jit/preloading`).

A few conventions worth knowing:

- **Docs are compiled as MDX.** Literal `<...>` and `{...}` in prose (type
  annotations, dict literals, placeholders) must be wrapped in backticks, or
  MDX parses them as JSX tags/expressions and the build fails.
- **Broken internal links fail the build** (`onBrokenLinks: 'throw'`). Always
  run `yarn build` before sending a change — CI runs the same check.
- **Internal docs are not published here.** Developer-only documentation lives
  outside this directory and is excluded from the public mirror; do not add
  internal content to `docs/`.

## Local development

```bash
cd cinderx/website
yarn install
yarn start      # dev server with hot reload at http://localhost:3000
```

## Build & preview

```bash
yarn build      # builds the static site into ./build (fails on broken links)
yarn serve      # serve the production build locally
yarn typecheck  # type-check the TypeScript config files
```

## Deployment

Deployment is automatic. The `.github/workflows/docs.yml` GitHub Actions
workflow builds the site on every pull request that touches `cinderx/website/`,
and builds and deploys it to GitHub Pages when those changes land on `main`.
</content>
</invoke>
