import { afterEach } from 'vitest'
import { cleanup } from '@testing-library/react'

// Vitest runs without globals, so Testing Library can't register its own cleanup.
afterEach(() => cleanup())
