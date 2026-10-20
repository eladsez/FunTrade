import { useOverview } from '../lib/api'
import { Card, Failed, Loading } from '../components/ui'
import { BasketList } from '../components/BasketList'

export default function Baskets() {
  const { data, error, isLoading } = useOverview()
  if (isLoading) return <Loading />
  if (error || !data) return <Failed error={error} />
  return <Card><BasketList rows={data.baskets} /></Card>
}
