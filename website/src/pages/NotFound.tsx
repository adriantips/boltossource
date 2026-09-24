import { Container, Button } from '@/components/site/Blocks'

export default function NotFound() {
  return (
    <section className="grid min-h-[80vh] place-items-center pt-24">
      <Container className="text-center">
        <p className="font-mono text-[13px] text-fog">#PF · page fault at an unmapped address</p>
        <h1 className="display mt-8 text-[clamp(5rem,20vw,16rem)]">404</h1>
        <p className="mx-auto mt-6 max-w-md text-[17px] text-smoke">The kernel would kill the process. We will just send you home.</p>
        <div className="mt-10 flex justify-center gap-3">
          <Button to="/">Back to BoltOS</Button>
          <Button to="/system" variant="ghost">Explore the system</Button>
        </div>
      </Container>
    </section>
  )
}
