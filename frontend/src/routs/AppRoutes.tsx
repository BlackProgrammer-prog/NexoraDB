import { lazy, Suspense } from 'react'
import { PageErrorBoundary } from '../shared/components/ui/PageErrorBoundary'
import { Navigate, Outlet, Route, Routes, useLocation } from 'react-router-dom'
import { useAuth } from '../features/auth/context/useAuth'
import { DashboardLayout } from '../layouts/DashboardLayout'

const AppTokensPage = lazy(() => import('../pages/AppTokensPage').then((module) => ({ default: module.AppTokensPage })))
const CollectionsPage = lazy(() => import('../pages/CollectionsPage').then((module) => ({ default: module.CollectionsPage })))
const DashboardPage = lazy(() => import('../pages/DashboardPage').then((module) => ({ default: module.DashboardPage })))
const DocumentsPage = lazy(() => import('../pages/DocumentsPage').then((module) => ({ default: module.DocumentsPage })))
const GraphsPage = lazy(() => import('../pages/GraphsPage').then((module) => ({ default: module.GraphsPage })))
const ForgotPasswordPage = lazy(() => import('../pages/auth/ForgotPasswordPage').then((module) => ({ default: module.ForgotPasswordPage })))
const LoginPage = lazy(() => import('../pages/auth/LoginPage').then((module) => ({ default: module.LoginPage })))
const RegisterPage = lazy(() => import('../pages/auth/RegisterPage').then((module) => ({ default: module.RegisterPage })))
const NotFoundPage = lazy(() => import('../pages/NotFoundPage').then((module) => ({ default: module.NotFoundPage })))
const QueryPage = lazy(() => import('../pages/QueryPage').then((module) => ({ default: module.QueryPage })))
const SecurityPage = lazy(() => import('../pages/SecurityPage').then((module) => ({ default: module.SecurityPage })))

function FullPageLoading() {
  return (
    <div className="flex min-h-screen items-center justify-center bg-slate-50 text-sm font-medium text-slate-600">
      Loading console...
    </div>
  )
}

function ProtectedRoute() {
  const { isAuthenticated, isInitializing } = useAuth()
  const location = useLocation()

  if (isInitializing) {
    return <FullPageLoading />
  }

  if (!isAuthenticated) {
    return <Navigate replace state={{ from: location }} to="/login" />
  }

  return <Outlet />
}

export function AppRoutes() {
  const location = useLocation()
  return (
    <PageErrorBoundary key={location.pathname}>
    <Suspense fallback={<FullPageLoading />}>
    <Routes>
      <Route element={<Navigate replace to="/login" />} path="/" />
      <Route element={<LoginPage />} path="/login" />
      <Route element={<RegisterPage />} path="/register" />
      <Route element={<ForgotPasswordPage />} path="/forgot-password" />
      <Route element={<ProtectedRoute />}>
        <Route element={<DashboardLayout />}>
          <Route element={<DashboardPage />} path="/dashboard" />
          <Route element={<CollectionsPage />} path="/collections" />
          <Route element={<DocumentsPage />} path="/documents" />
          <Route element={<GraphsPage />} path="/graphs" />
          <Route element={<QueryPage />} path="/query" />
          <Route element={<AppTokensPage />} path="/app-tokens" />
          <Route element={<SecurityPage />} path="/security" />
        </Route>
      </Route>
      <Route element={<NotFoundPage />} path="*" />
    </Routes>
    </Suspense>
    </PageErrorBoundary>
  )
}
