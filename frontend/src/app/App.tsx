import { useState } from 'react'
import { DashboardLayout, type AppPage } from '../layouts/DashboardLayout'
import { CollectionsPage } from '../pages/CollectionsPage'
import { DashboardPage } from '../pages/DashboardPage'
import { DocumentsPage } from '../pages/DocumentsPage'
import { NotFoundPage } from '../pages/NotFoundPage'

function renderPage(page: AppPage) {
  switch (page) {
    case 'dashboard':
      return <DashboardPage />
    case 'collections':
      return <CollectionsPage />
    case 'documents':
      return <DocumentsPage />
    default:
      return <NotFoundPage />
  }
}

export default function App() {
  const [activePage, setActivePage] = useState<AppPage>('dashboard')

  return (
    <DashboardLayout activePage={activePage} onNavigate={setActivePage}>
      {renderPage(activePage)}
    </DashboardLayout>
  )
}
