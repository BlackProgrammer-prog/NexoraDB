import { Navigate, Route, Routes } from 'react-router-dom'
import { DashboardLayout } from '../layouts/DashboardLayout'
import { CollectionsPage } from '../pages/CollectionsPage'
import { DashboardPage } from '../pages/DashboardPage'
import { DocumentsPage } from '../pages/DocumentsPage'
import { GraphsPage } from '../pages/GraphsPage'
import { ForgotPasswordPage } from '../pages/auth/ForgotPasswordPage'
import { LoginPage } from '../pages/auth/LoginPage'
import { RegisterPage } from '../pages/auth/RegisterPage'
import { NotFoundPage } from '../pages/NotFoundPage'
import { QueryPage } from '../pages/QueryPage'

export function AppRoutes() {
  return (
    <Routes>
      <Route element={<Navigate replace to="/login" />} path="/" />
      <Route element={<LoginPage />} path="/login" />
      <Route element={<RegisterPage />} path="/register" />
      <Route element={<ForgotPasswordPage />} path="/forgot-password" />
      <Route element={<DashboardLayout />}>
        <Route element={<DashboardPage />} path="/dashboard" />
        <Route element={<CollectionsPage />} path="/collections" />
        <Route element={<DocumentsPage />} path="/documents" />
        <Route element={<GraphsPage />} path="/graphs" />
        <Route element={<QueryPage />} path="/query" />
      </Route>
      <Route element={<NotFoundPage />} path="*" />
    </Routes>
  )
}
