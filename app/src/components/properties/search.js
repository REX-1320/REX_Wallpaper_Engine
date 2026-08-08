import { createContext, useContext, Children, isValidElement } from 'react'

export const InspectorSearchContext = createContext('')

export function useInspectorSearch() {
  return useContext(InspectorSearchContext)
}

export function matchesQuery(children, query) {
  if (!query) return Children.toArray(children)
  return Children.toArray(children).filter((child) => {
    if (!isValidElement(child)) return false
    const label = child.props?.label ?? child.props?.title ?? ''
    return label.toLowerCase().includes(query)
  })
}
