import { cn } from '../../../shared/utils/cn'
import type { AppScopeDefinition } from '../types/appToken.types'

interface AppScopeSelectorProps {
  availableScopes: string[]
  catalog: AppScopeDefinition[]
  selectedScopes: string[]
  onChange: (scopes: string[]) => void
}

export function AppScopeSelector({
  availableScopes,
  catalog,
  selectedScopes,
  onChange,
}: AppScopeSelectorProps) {
  const visibleCatalog = catalog.filter((item) => availableScopes.includes(item.scope))

  function toggleScope(scope: string) {
    if (selectedScopes.includes(scope)) {
      onChange(selectedScopes.filter((item) => item !== scope))
      return
    }
    onChange([...selectedScopes, scope])
  }

  return (
    <div className="grid gap-3 sm:grid-cols-2">
      {visibleCatalog.map((item) => {
        const isSelected = selectedScopes.includes(item.scope)

        return (
          <label
            className={cn(
              'flex cursor-pointer gap-3 rounded-lg border p-4 transition',
              isSelected
                ? 'border-green-300 bg-green-50'
                : 'border-slate-200 bg-white hover:border-slate-300',
            )}
            key={item.scope}
          >
            <input
              checked={isSelected}
              className="mt-1 size-4 accent-green-600"
              onChange={() => toggleScope(item.scope)}
              type="checkbox"
            />
            <span>
              <span className="block text-sm font-semibold text-slate-950">{item.label}</span>
              <span className="mt-1 block font-mono text-xs text-green-700">{item.scope}</span>
              <span className="mt-2 block text-xs leading-5 text-slate-500">
                {item.description}
              </span>
            </span>
          </label>
        )
      })}
    </div>
  )
}
