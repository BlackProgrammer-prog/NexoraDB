import { mockQueryExamples } from '../../../mocks/mockQueries'
import { Button } from '../../../shared/components/ui/Button'

interface QueryExamplesProps {
  onSelect: (query: string) => void
}

export function QueryExamples({ onSelect }: QueryExamplesProps) {
  return (
    <div className="flex flex-wrap gap-2">
      {mockQueryExamples.map((query) => (
        <Button
          className="h-8 px-3 text-xs"
          key={query}
          onClick={() => onSelect(query)}
          variant="secondary"
        >
          {query}
        </Button>
      ))}
    </div>
  )
}
