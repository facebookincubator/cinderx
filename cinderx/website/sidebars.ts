import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

// Hand-authored sidebar. Doc ids are the file paths under `website/docs`
// (minus extension), e.g. `StaticPython/index`, `Jit/guide`.
const sidebars: SidebarsConfig = {
  docsSidebar: [
    'introduction',
    {
      type: 'category',
      label: 'JIT Compiler',
      collapsed: false,
      link: {type: 'doc', id: 'Jit/index'},
      items: [
        'Jit/preloading',
        {
          type: 'category',
          label: 'Internals',
          items: [
            'Jit/guide',
            'Jit/deoptimization',
            'Jit/hir/type',
            'Jit/hir/refcount_insertion',
          ],
        },
      ],
    },
    {
      type: 'category',
      label: 'Static Python',
      collapsed: false,
      link: {type: 'doc', id: 'StaticPython/index'},
      items: [
        'StaticPython/tutorial',
        'StaticPython/incompatibilities',
      ],
    },
    'FAQ',
  ],
};

export default sidebars;
