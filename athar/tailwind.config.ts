import type { Config } from 'tailwindcss';

const config: Config = {
  content: ['./src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        ink: {
          950: '#07090d',
          900: '#0b0e14',
          850: '#11151d',
          800: '#171c26',
          700: '#232a38',
          600: '#374153'
        },
        athar: {
          400: '#4ade80',
          500: '#22c55e',
          600: '#16a34a'
        },
        amber: {
          glow: '#f5b544'
        }
      },
      fontFamily: {
        sans: ['var(--font-sans)', 'system-ui', 'sans-serif'],
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'monospace']
      }
    }
  },
  plugins: []
};

export default config;
