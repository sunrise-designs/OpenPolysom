import tseslint from 'typescript-eslint';
import functional from 'eslint-plugin-functional';

export default tseslint.config(
  {
    ignores: ['node_modules/**', 'dist/**', 'coverage/**'],
  },
  {
    files: ['**/*.{ts,tsx}'],
    extends: [
      ...tseslint.configs.strictTypeChecked,
      ...tseslint.configs.stylisticTypeChecked,
    ],
    plugins: {
      functional,
    },
    languageOptions: {
      parserOptions: {
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
    rules: {
      'functional/immutable-data': 'error',
      'functional/no-let': 'error',
      'functional/prefer-readonly-type': 'error',
      '@typescript-eslint/prefer-readonly': 'error',
      '@typescript-eslint/prefer-readonly-parameter-types': 'error',
    },
  },
  {
    // Functional core / imperative shell. The strict functional + readonly-parameter
    // rules hold on the pure core (format / narrative / tokens / types). They are
    // relaxed on the DOM/ECharts/Zarr I/O edge and tests, where typed-array, DOM and
    // ECharts-option parameters (and a little local mutation in the chart lifecycle)
    // are unavoidable.
    files: ['src/main.ts', 'src/chart.ts', 'src/shell.ts', 'src/landing.ts', 'src/zarr_loader.ts', 'test/**/*.ts'],
    rules: {
      '@typescript-eslint/prefer-readonly-parameter-types': 'off',
      'functional/prefer-readonly-type': 'off',
      'functional/immutable-data': 'off',
      'functional/no-let': 'off',
    },
  },
);
