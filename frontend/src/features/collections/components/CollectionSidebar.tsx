import { useCollections } from '../hooks/useCollections'

export function CollectionSidebar() {
  const { data: collections } = useCollections()

  return (
    <aside className="rounded-lg border border-slate-200 bg-white p-4">
      <h2 className="text-sm font-semibold text-slate-950">Collections</h2>
      <div className="mt-4 space-y-2">
        {(collections ?? []).map((collection) => (
          <div
            className="rounded-md px-3 py-2 text-sm text-slate-600 hover:bg-green-50 hover:text-green-700"
            key={collection.name}
          >
            {collection.name}
          </div>
        ))}
      </div>
    </aside>
  )
}
