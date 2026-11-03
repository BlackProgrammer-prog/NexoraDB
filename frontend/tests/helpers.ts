export function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: unknown) => void
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

export const admin = {
  _id: 'root', username: 'root', email: 'root@example.test', role: 'admin' as const,
  firstName: 'Test', lastName: 'Admin', status: 'active' as const,
  createdAt: 1, updatedAt: 1, lastLoginAt: 1, displayName: 'Test Admin',
}
