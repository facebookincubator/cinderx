import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// CinderX documentation site. All documentation lives in `website/docs/`.

const config: Config = {
  title: 'CinderX',
  tagline: 'A Python extension that improves the performance of the Python runtime',
  favicon: 'img/flame.png',

  // Production url of the site, deployed to GitHub Pages.
  url: 'https://facebookincubator.github.io',
  // The site is served from the `/cinderx/` project-pages path.
  baseUrl: '/cinderx/',

  // GitHub Pages deployment config.
  organizationName: 'facebookincubator',
  projectName: 'cinderx',
  trailingSlash: false,

  // Fail the build on broken internal links so doc rot is caught in CI.
  onBrokenLinks: 'throw',

  markdown: {
    // Compile all docs as MDX. Literal `<...>` and `{...}` in prose must be
    // wrapped in backticks (inline code), otherwise they are parsed as JSX
    // tags or expressions and fail the build.
    format: 'mdx',
    // Render ```mermaid fenced code blocks as diagrams (requires the
    // `@docusaurus/theme-mermaid` theme registered below).
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
      // Images are kept lenient: some upstream docs reference figures that are
      // not synced to OSS. Broken images warn rather than fail the build.
      onBrokenMarkdownImages: 'warn',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  plugins: [
    // Generate AI-agent-friendly artifacts: llms.txt (index), llms-full.txt
    // (full corpus), and a raw .md for every page.
    [
      'docusaurus-plugin-llms',
      {
        title: 'CinderX Documentation',
        description:
          'CinderX is a Python extension that improves the performance of the Python runtime, featuring a JIT compiler and Static Python.',
        generateLLMsTxt: true,
        generateLLMsFullTxt: true,
        generateMarkdownFiles: true,
        keepFrontMatter: ['title', 'description'],
      },
    ],
  ],

  // Adds Mermaid rendering support for ```mermaid code blocks (see
  // `markdown.mermaid` above).
  themes: ['@docusaurus/theme-mermaid'],

  presets: [
    [
      'classic',
      {
        docs: {
          // Docs live in the default `website/docs/` directory. Serve them at
          // the site root so visitors land on the Introduction (slug `/`).
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
          editUrl:
            'https://github.com/facebookincubator/cinderx/tree/main/cinderx/website/',
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
        sitemap: {
          changefreq: 'weekly',
          priority: 0.5,
          filename: 'sitemap.xml',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    image: 'img/logo.png',
    navbar: {
      // No `title` here: the logo is the full CinderX wordmark, so adding a text
      // title would render "cinderx" twice. The flame-only mark is the favicon.
      logo: {
        alt: 'CinderX',
        src: 'img/logo.png',
        srcDark: 'img/logo-dark.png',
      },
      items: [
        {
          href: 'https://pypi.org/project/cinderx/',
          label: 'PyPI',
          position: 'right',
        },
        {
          href: 'https://github.com/facebookincubator/cinderx',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Introduction', to: '/'},
            {label: 'JIT Compiler', to: '/jit'},
            {label: 'Static Python', to: '/static-python'},
          ],
        },
        {
          title: 'Community',
          items: [
            {
              label: 'GitHub',
              href: 'https://github.com/facebookincubator/cinderx',
            },
            {label: 'PyPI', href: 'https://pypi.org/project/cinderx/'},
          ],
        },
        {
          title: 'More',
          items: [
            {label: 'Terms of Use', href: 'https://opensource.fb.com/legal/terms'},
            {
              label: 'Privacy Policy',
              href: 'https://opensource.fb.com/legal/privacy',
            },
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Meta Platforms, Inc. Built with Docusaurus.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['python', 'bash', 'nasm', 'json'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
