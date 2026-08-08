import { useState } from 'react'
import { toast } from 'sonner'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { z } from 'zod'
import { BarChart, Bar, CartesianGrid, XAxis } from 'recharts'
import { FileText, Star, Home, Inbox, Settings } from 'lucide-react'

import { Accordion, AccordionItem, AccordionTrigger, AccordionContent } from './components/ui/accordion'
import { Alert, AlertTitle, AlertDescription } from './components/ui/alert'
import { AlertDialog, AlertDialogTrigger, AlertDialogContent, AlertDialogHeader, AlertDialogTitle, AlertDialogDescription, AlertDialogFooter, AlertDialogCancel, AlertDialogAction } from './components/ui/alert-dialog'
import { AspectRatio } from './components/ui/aspect-ratio'
import { Attachment, AttachmentMedia, AttachmentContent, AttachmentTitle, AttachmentDescription } from './components/ui/attachment'
import { Avatar, AvatarFallback } from './components/ui/avatar'
import { Badge } from './components/ui/badge'
import { Breadcrumb, BreadcrumbList, BreadcrumbItem, BreadcrumbLink, BreadcrumbSeparator, BreadcrumbPage } from './components/ui/breadcrumb'
import { Bubble, BubbleContent } from './components/ui/bubble'
import { Button } from './components/ui/button'
import { ButtonGroup } from './components/ui/button-group'
import { Calendar } from './components/ui/calendar'
import { Card, CardHeader, CardTitle, CardDescription, CardContent } from './components/ui/card'
import { Carousel, CarouselContent, CarouselItem, CarouselPrevious, CarouselNext } from './components/ui/carousel'
import { ChartContainer, ChartTooltip, ChartTooltipContent } from './components/ui/chart'
import { Checkbox } from './components/ui/checkbox'
import { Collapsible, CollapsibleTrigger, CollapsibleContent } from './components/ui/collapsible'
import { Combobox, ComboboxInput, ComboboxContent, ComboboxList, ComboboxItem } from './components/ui/combobox'
import { Command, CommandInput, CommandList, CommandEmpty, CommandGroup, CommandItem } from './components/ui/command'
import { ContextMenu, ContextMenuTrigger, ContextMenuContent, ContextMenuItem } from './components/ui/context-menu'
import { Dialog, DialogTrigger, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter, DialogClose } from './components/ui/dialog'
import { DirectionProvider, useDirection } from './components/ui/direction'
import { Drawer, DrawerTrigger, DrawerContent, DrawerHeader, DrawerTitle, DrawerDescription, DrawerFooter, DrawerClose } from './components/ui/drawer'
import { DropdownMenu, DropdownMenuTrigger, DropdownMenuContent, DropdownMenuLabel, DropdownMenuItem, DropdownMenuSeparator } from './components/ui/dropdown-menu'
import { Empty, EmptyMedia, EmptyHeader, EmptyTitle, EmptyDescription } from './components/ui/empty'
import { Field, FieldLabel, FieldDescription, FieldError } from './components/ui/field'
import { Form, FormField, FormItem, FormLabel, FormControl, FormMessage } from './components/ui/form'
import { HoverCard, HoverCardTrigger, HoverCardContent } from './components/ui/hover-card'
import { Input } from './components/ui/input'
import { InputGroup, InputGroupText, InputGroupInput } from './components/ui/input-group'
import { InputOTP, InputOTPGroup, InputOTPSlot } from './components/ui/input-otp'
import { Item, ItemMedia, ItemContent, ItemTitle, ItemGroup } from './components/ui/item'
import { Kbd } from './components/ui/kbd'
import { Label } from './components/ui/label'
import { Marker, MarkerIcon, MarkerContent } from './components/ui/marker'
import { Menubar, MenubarMenu, MenubarTrigger, MenubarContent, MenubarItem, MenubarSeparator } from './components/ui/menubar'
import { MessageGroup, Message, MessageHeader, MessageAvatar, MessageContent } from './components/ui/message'
import { MessageScrollerProvider, MessageScroller, MessageScrollerViewport, MessageScrollerContent, MessageScrollerItem } from './components/ui/message-scroller'
import { NativeSelect } from './components/ui/native-select'
import { NavigationMenu, NavigationMenuList, NavigationMenuItem, NavigationMenuTrigger, NavigationMenuContent, NavigationMenuLink } from './components/ui/navigation-menu'
import { Pagination, PaginationContent, PaginationItem, PaginationPrevious, PaginationLink, PaginationNext, PaginationEllipsis } from './components/ui/pagination'
import { Popover, PopoverTrigger, PopoverContent, PopoverHeader, PopoverTitle, PopoverDescription } from './components/ui/popover'
import { Progress } from './components/ui/progress'
import { RadioGroup, RadioGroupItem } from './components/ui/radio-group'
import { ResizablePanelGroup, ResizablePanel, ResizableHandle } from './components/ui/resizable'
import { ScrollArea } from './components/ui/scroll-area'
import { Select, SelectTrigger, SelectValue, SelectContent, SelectItem } from './components/ui/select'
import { Separator } from './components/ui/separator'
import { Sheet, SheetTrigger, SheetContent, SheetHeader, SheetTitle, SheetDescription, SheetFooter, SheetClose } from './components/ui/sheet'
import { SidebarProvider, Sidebar, SidebarHeader, SidebarContent, SidebarGroup, SidebarGroupLabel, SidebarGroupContent, SidebarMenu, SidebarMenuItem, SidebarMenuButton } from './components/ui/sidebar'
import { Skeleton } from './components/ui/skeleton'
import { Slider } from './components/ui/slider'
import { Toaster } from './components/ui/sonner'
import { Spinner } from './components/ui/spinner'
import { Switch } from './components/ui/switch'
import { Table, TableHeader, TableBody, TableRow, TableHead, TableCell } from './components/ui/table'
import { Tabs, TabsList, TabsTrigger, TabsContent } from './components/ui/tabs'
import { Textarea } from './components/ui/textarea'
import { Toggle } from './components/ui/toggle'
import { ToggleGroup, ToggleGroupItem } from './components/ui/toggle-group'
import { TooltipProvider, Tooltip, TooltipTrigger, TooltipContent } from './components/ui/tooltip'

function Tile({ name, children }) {
  return (
    <div className="shadcn-tile">
      <div className="shadcn-tile-head">{name}</div>
      <div className="shadcn-tile-body">{children}</div>
    </div>
  )
}

function Section({ title, children }) {
  return (
    <div className="shadcn-section">
      <h3 className="shadcn-section-title">{title}</h3>
      <div className="shadcn-grid">{children}</div>
    </div>
  )
}

const schema = z.object({ name: z.string().min(2, 'Name must be at least 2 characters') })

function DirText() {
  const dir = useDirection()
  return <p className="text-sm">Current direction: {dir}</p>
}

const chartData = [
  { month: 'Jan', desktop: 186 },
  { month: 'Feb', desktop: 305 },
  { month: 'Mar', desktop: 237 },
  { month: 'Apr', desktop: 73 },
]

const chartConfig = {
  desktop: { label: 'Desktop', color: '#2563eb' },
}

const NAV_LINKS = [
  { label: 'Home', icon: Home },
  { label: 'Inbox', icon: Inbox },
  { label: 'Settings', icon: Settings },
]

function ShadcnGallery() {
  const [calDate, setCalDate] = useState()
  const [collapsibleOpen, setCollapsibleOpen] = useState(true)
  const form = useForm({ resolver: zodResolver(schema), defaultValues: { name: '' } })

  return (
    <>
      <Toaster position="top-right" richColors />
      <Section title="Form controls">
        <Tile name="button">
          <div className="row">
            <Button>Default</Button>
            <Button variant="outline">Outline</Button>
            <Button variant="destructive" size="sm">Delete</Button>
          </div>
        </Tile>
        <Tile name="button-group">
          <ButtonGroup>
            <Button variant="outline">Bold</Button>
            <Button variant="outline">Italic</Button>
            <Button variant="outline">Underline</Button>
          </ButtonGroup>
        </Tile>
        <Tile name="checkbox">
          <div className="flex items-center gap-2">
            <Checkbox id="agree" />
            <Label htmlFor="agree">Accept terms</Label>
          </div>
        </Tile>
        <Tile name="combobox">
          <Combobox>
            <ComboboxInput placeholder="Pick a fruit…" />
            <ComboboxContent>
              <ComboboxList>
                <ComboboxItem value="apple">Apple</ComboboxItem>
                <ComboboxItem value="banana">Banana</ComboboxItem>
                <ComboboxItem value="cherry">Cherry</ComboboxItem>
              </ComboboxList>
            </ComboboxContent>
          </Combobox>
        </Tile>
        <Tile name="field">
          <Field>
            <FieldLabel>Email</FieldLabel>
            <FieldDescription>We never share your email.</FieldDescription>
            <Input placeholder="you@example.com" />
            <FieldError>Required field</FieldError>
          </Field>
        </Tile>
        <Tile name="input">
          <Input placeholder="Type something…" />
        </Tile>
        <Tile name="input-group">
          <InputGroup>
            <InputGroupText>https://</InputGroupText>
            <InputGroupInput placeholder="example.com" />
          </InputGroup>
        </Tile>
        <Tile name="input-otp">
          <InputOTP maxLength={4}>
            <InputOTPGroup>
              <InputOTPSlot index={0} />
              <InputOTPSlot index={1} />
              <InputOTPSlot index={2} />
              <InputOTPSlot index={3} />
            </InputOTPGroup>
          </InputOTP>
        </Tile>
        <Tile name="label">
          <div className="flex flex-col gap-1.5">
            <Label htmlFor="name">Name</Label>
            <Input id="name" placeholder="Jane Doe" />
          </div>
        </Tile>
        <Tile name="native-select">
          <NativeSelect>
            <option>Apples</option>
            <option>Bananas</option>
            <option>Cherries</option>
          </NativeSelect>
        </Tile>
        <Tile name="radio-group">
          <RadioGroup defaultValue="a">
            <div className="flex items-center gap-2">
              <RadioGroupItem value="a" id="ra" />
              <Label htmlFor="ra">Option A</Label>
            </div>
            <div className="flex items-center gap-2">
              <RadioGroupItem value="b" id="rb" />
              <Label htmlFor="rb">Option B</Label>
            </div>
          </RadioGroup>
        </Tile>
        <Tile name="select">
          <Select defaultValue="apple">
            <SelectTrigger className="w-40">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="apple">Apple</SelectItem>
              <SelectItem value="banana">Banana</SelectItem>
              <SelectItem value="cherry">Cherry</SelectItem>
            </SelectContent>
          </Select>
        </Tile>
        <Tile name="slider">
          <Slider defaultValue={[40]} className="w-40" />
        </Tile>
        <Tile name="switch">
          <div className="flex items-center gap-2">
            <Switch id="notif" />
            <Label htmlFor="notif">Notifications</Label>
          </div>
        </Tile>
        <Tile name="textarea">
          <Textarea placeholder="Write a message…" />
        </Tile>
        <Tile name="toggle">
          <div className="row">
            <Toggle aria-label="Bold">B</Toggle>
            <Toggle aria-label="Italic">I</Toggle>
          </div>
        </Tile>
        <Tile name="toggle-group">
          <ToggleGroup type="single" defaultValue="a">
            <ToggleGroupItem value="a">A</ToggleGroupItem>
            <ToggleGroupItem value="b">B</ToggleGroupItem>
            <ToggleGroupItem value="c">C</ToggleGroupItem>
          </ToggleGroup>
        </Tile>
      </Section>

      <Section title="Feedback">
        <Tile name="alert">
          <Alert>
            <AlertTitle>Heads up!</AlertTitle>
            <AlertDescription>You can add components to your app.</AlertDescription>
          </Alert>
        </Tile>
        <Tile name="badge">
          <div className="row">
            <Badge>Default</Badge>
            <Badge variant="secondary">Secondary</Badge>
            <Badge variant="outline">Outline</Badge>
            <Badge variant="destructive">Destructive</Badge>
          </div>
        </Tile>
        <Tile name="bubble">
          <Bubble variant="default" align="end">
            <BubbleContent>Hey! How are you?</BubbleContent>
          </Bubble>
          <Bubble variant="muted" align="start">
            <BubbleContent>All good, thanks!</BubbleContent>
          </Bubble>
        </Tile>
        <Tile name="empty">
          <Empty>
            <EmptyMedia variant="icon">
              <Star />
            </EmptyMedia>
            <EmptyHeader>
              <EmptyTitle>No results</EmptyTitle>
              <EmptyDescription>Try adjusting your search to find what you are looking for.</EmptyDescription>
            </EmptyHeader>
          </Empty>
        </Tile>
        <Tile name="kbd">
          <div className="row">
            <Kbd>Ctrl</Kbd>
            <span className="text-xs text-muted-foreground">+</span>
            <Kbd>K</Kbd>
          </div>
        </Tile>
        <Tile name="marker">
          <Marker>
            <MarkerIcon>
              <Star />
            </MarkerIcon>
            <MarkerContent>Reviewed 3 hours ago</MarkerContent>
          </Marker>
          <Marker variant="separator">
            <MarkerContent>New</MarkerContent>
          </Marker>
        </Tile>
        <Tile name="message">
          <MessageGroup>
            <Message align="start">
              <MessageAvatar>JD</MessageAvatar>
              <MessageContent>
                <MessageHeader>Jane Doe</MessageHeader>
                <p className="rounded-lg bg-muted px-3 py-2">Hello there!</p>
              </MessageContent>
            </Message>
            <Message align="end">
              <MessageContent>
                <p className="rounded-lg bg-primary px-3 py-2 text-primary-foreground">Hi Jane!</p>
              </MessageContent>
            </Message>
          </MessageGroup>
        </Tile>
        <Tile name="message-scroller">
          <div className="h-32 w-full">
            <MessageScrollerProvider>
              <MessageScroller>
                <MessageScrollerViewport>
                  <MessageScrollerContent>
                    {Array.from({ length: 8 }, (_, i) => (
                      <MessageScrollerItem key={i} scrollAnchor={i === 7}>
                        <p className="text-xs text-muted-foreground">Message {i + 1} — a short line of chat content.</p>
                      </MessageScrollerItem>
                    ))}
                  </MessageScrollerContent>
                </MessageScrollerViewport>
              </MessageScroller>
            </MessageScrollerProvider>
          </div>
        </Tile>
        <Tile name="progress">
          <Progress value={60} className="w-40" />
        </Tile>
        <Tile name="skeleton">
          <div className="flex flex-col gap-2 w-40">
            <Skeleton className="h-3 w-full" />
            <Skeleton className="h-3 w-3/4" />
            <Skeleton className="h-8 w-full" />
          </div>
        </Tile>
        <Tile name="sonner">
          <Button onClick={() => toast('Hello from sonner!')}>Fire a toast</Button>
        </Tile>
        <Tile name="spinner">
          <div className="row">
            <Spinner />
            <span className="text-sm text-muted-foreground">Loading…</span>
          </div>
        </Tile>
      </Section>

      <Section title="Overlays">
        <Tile name="alert-dialog">
          <AlertDialog>
            <AlertDialogTrigger asChild>
              <Button variant="outline">Delete account</Button>
            </AlertDialogTrigger>
            <AlertDialogContent>
              <AlertDialogHeader>
                <AlertDialogTitle>Are you sure?</AlertDialogTitle>
                <AlertDialogDescription>This action cannot be undone.</AlertDialogDescription>
              </AlertDialogHeader>
              <AlertDialogFooter>
                <AlertDialogCancel>Cancel</AlertDialogCancel>
                <AlertDialogAction>Continue</AlertDialogAction>
              </AlertDialogFooter>
            </AlertDialogContent>
          </AlertDialog>
        </Tile>
        <Tile name="command">
          <Command className="w-full">
            <CommandInput placeholder="Type a command…" />
            <CommandList>
              <CommandEmpty>No results found.</CommandEmpty>
              <CommandGroup heading="Suggestions">
                <CommandItem>Calendar</CommandItem>
                <CommandItem>Search Emoji</CommandItem>
              </CommandGroup>
            </CommandList>
          </Command>
        </Tile>
        <Tile name="context-menu">
          <ContextMenu>
            <ContextMenuTrigger asChild>
              <div className="flex h-20 w-full items-center justify-center rounded-md border border-dashed text-sm text-muted-foreground">
                Right click here
              </div>
            </ContextMenuTrigger>
            <ContextMenuContent>
              <ContextMenuItem>Cut</ContextMenuItem>
              <ContextMenuItem>Copy</ContextMenuItem>
              <ContextMenuItem>Paste</ContextMenuItem>
            </ContextMenuContent>
          </ContextMenu>
        </Tile>
        <Tile name="dialog">
          <Dialog>
            <DialogTrigger asChild>
              <Button variant="outline">Open dialog</Button>
            </DialogTrigger>
            <DialogContent>
              <DialogHeader>
                <DialogTitle>Edit profile</DialogTitle>
                <DialogDescription>Make changes to your profile here.</DialogDescription>
              </DialogHeader>
              <Input placeholder="Your name" />
              <DialogFooter>
                <DialogClose asChild>
                  <Button variant="outline">Close</Button>
                </DialogClose>
                <Button>Save</Button>
              </DialogFooter>
            </DialogContent>
          </Dialog>
        </Tile>
        <Tile name="dropdown-menu">
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="outline">Open menu</Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent>
              <DropdownMenuLabel>My Account</DropdownMenuLabel>
              <DropdownMenuItem>Profile</DropdownMenuItem>
              <DropdownMenuItem>Billing</DropdownMenuItem>
              <DropdownMenuSeparator />
              <DropdownMenuItem className="text-destructive">Log out</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </Tile>
        <Tile name="drawer">
          <Drawer>
            <DrawerTrigger asChild>
              <Button variant="outline">Open drawer</Button>
            </DrawerTrigger>
            <DrawerContent>
              <DrawerHeader>
                <DrawerTitle>Are you sure?</DrawerTitle>
                <DrawerDescription>This action cannot be undone.</DrawerDescription>
              </DrawerHeader>
              <DrawerFooter>
                <Button>Confirm</Button>
                <DrawerClose asChild>
                  <Button variant="outline">Cancel</Button>
                </DrawerClose>
              </DrawerFooter>
            </DrawerContent>
          </Drawer>
        </Tile>
        <Tile name="hover-card">
          <HoverCard>
            <HoverCardTrigger asChild>
              <Button variant="link">@example</Button>
            </HoverCardTrigger>
            <HoverCardContent>
              <p className="text-sm">The React Framework</p>
            </HoverCardContent>
          </HoverCard>
        </Tile>
        <Tile name="menubar">
          <Menubar>
            <MenubarMenu>
              <MenubarTrigger>File</MenubarTrigger>
              <MenubarContent>
                <MenubarItem>New Tab</MenubarItem>
                <MenubarItem>New Window</MenubarItem>
                <MenubarSeparator />
                <MenubarItem>Quit</MenubarItem>
              </MenubarContent>
            </MenubarMenu>
          </Menubar>
        </Tile>
        <Tile name="navigation-menu">
          <NavigationMenu>
            <NavigationMenuList>
              <NavigationMenuItem>
                <NavigationMenuTrigger>Learn</NavigationMenuTrigger>
                <NavigationMenuContent>
                  <div className="flex w-44 flex-col gap-1 p-3 text-sm">
                    <NavigationMenuLink>Getting started</NavigationMenuLink>
                    <NavigationMenuLink>API reference</NavigationMenuLink>
                  </div>
                </NavigationMenuContent>
              </NavigationMenuItem>
            </NavigationMenuList>
          </NavigationMenu>
        </Tile>
        <Tile name="pagination">
          <Pagination>
            <PaginationContent>
              <PaginationItem>
                <PaginationPrevious href="#" />
              </PaginationItem>
              <PaginationItem>
                <PaginationLink href="#">1</PaginationLink>
              </PaginationItem>
              <PaginationItem>
                <PaginationLink href="#" isActive>2</PaginationLink>
              </PaginationItem>
              <PaginationItem>
                <PaginationEllipsis />
              </PaginationItem>
              <PaginationItem>
                <PaginationNext href="#" />
              </PaginationItem>
            </PaginationContent>
          </Pagination>
        </Tile>
        <Tile name="popover">
          <Popover>
            <PopoverTrigger asChild>
              <Button variant="outline">Open popover</Button>
            </PopoverTrigger>
            <PopoverContent>
              <PopoverHeader>
                <PopoverTitle>Notifications</PopoverTitle>
                <PopoverDescription>You have 3 unread messages.</PopoverDescription>
              </PopoverHeader>
            </PopoverContent>
          </Popover>
        </Tile>
        <Tile name="sheet">
          <Sheet>
            <SheetTrigger asChild>
              <Button variant="outline">Open sheet</Button>
            </SheetTrigger>
            <SheetContent>
              <SheetHeader>
                <SheetTitle>Settings</SheetTitle>
                <SheetDescription>Adjust your preferences here.</SheetDescription>
              </SheetHeader>
              <div className="flex flex-col gap-2">
                <Button>Save changes</Button>
                <SheetClose asChild>
                  <Button variant="outline">Close</Button>
                </SheetClose>
              </div>
              <SheetFooter />
            </SheetContent>
          </Sheet>
        </Tile>
        <Tile name="tooltip">
          <TooltipProvider>
            <Tooltip>
              <TooltipTrigger asChild>
                <Button variant="outline">Hover me</Button>
              </TooltipTrigger>
              <TooltipContent>This is a tooltip</TooltipContent>
            </Tooltip>
          </TooltipProvider>
        </Tile>
      </Section>

      <Section title="Data display">
        <Tile name="accordion">
          <Accordion type="single" collapsible>
            <AccordionItem value="a">
              <AccordionTrigger>Is it accessible?</AccordionTrigger>
              <AccordionContent>Yes. It uses Radix primitives.</AccordionContent>
            </AccordionItem>
            <AccordionItem value="b">
              <AccordionTrigger>Is it styled?</AccordionTrigger>
              <AccordionContent>Yes. It comes with defaults.</AccordionContent>
            </AccordionItem>
          </Accordion>
        </Tile>
        <Tile name="aspect-ratio">
          <AspectRatio ratio={16 / 9} className="overflow-hidden rounded-md bg-muted">
            <div className="flex h-full w-full items-center justify-center text-sm text-muted-foreground">16:9</div>
          </AspectRatio>
        </Tile>
        <Tile name="avatar">
          <Avatar>
            <AvatarFallback>JD</AvatarFallback>
          </Avatar>
        </Tile>
        <Tile name="breadcrumb">
          <Breadcrumb>
            <BreadcrumbList>
              <BreadcrumbItem>
                <BreadcrumbLink href="#">Home</BreadcrumbLink>
              </BreadcrumbItem>
              <BreadcrumbSeparator />
              <BreadcrumbItem>
                <BreadcrumbLink href="#">Components</BreadcrumbLink>
              </BreadcrumbItem>
              <BreadcrumbSeparator />
              <BreadcrumbItem>
                <BreadcrumbPage>Breadcrumb</BreadcrumbPage>
              </BreadcrumbItem>
            </BreadcrumbList>
          </Breadcrumb>
        </Tile>
        <Tile name="calendar">
          <Calendar mode="single" selected={calDate} onSelect={setCalDate} className="w-fit rounded-md border" />
        </Tile>
        <Tile name="card">
          <Card className="w-full">
            <CardHeader>
              <CardTitle>Project</CardTitle>
              <CardDescription>Your latest project summary.</CardDescription>
            </CardHeader>
            <CardContent>
              <p className="text-sm">12 tasks · 3 done · 9 pending</p>
            </CardContent>
          </Card>
        </Tile>
        <Tile name="carousel">
          <Carousel className="w-full max-w-48">
            <CarouselContent>
              {['A', 'B', 'C'].map((item) => (
                <CarouselItem key={item}>
                  <div className="flex h-24 items-center justify-center rounded-md bg-muted text-2xl font-bold">{item}</div>
                </CarouselItem>
              ))}
            </CarouselContent>
            <CarouselPrevious className="-left-4" />
            <CarouselNext className="-right-4" />
          </Carousel>
        </Tile>
        <Tile name="chart">
          <ChartContainer config={chartConfig} className="h-36 w-full aspect-auto">
            <BarChart data={chartData}>
              <CartesianGrid vertical={false} />
              <XAxis dataKey="month" tickLine={false} axisLine={false} />
              <ChartTooltip content={<ChartTooltipContent />} />
              <Bar dataKey="desktop" fill="var(--color-desktop)" radius={4} />
            </BarChart>
          </ChartContainer>
        </Tile>
        <Tile name="collapsible">
          <Collapsible open={collapsibleOpen} onOpenChange={setCollapsibleOpen}>
            <div className="row">
              <CollapsibleTrigger asChild>
                <Button variant="outline" size="sm">{collapsibleOpen ? 'Hide' : 'Show'}</Button>
              </CollapsibleTrigger>
              <span className="text-sm text-muted-foreground">Collapsible section</span>
            </div>
            <CollapsibleContent className="mt-2">
              <p className="rounded-md bg-muted p-3 text-sm">@shadcn, this is the collapsible content.</p>
            </CollapsibleContent>
          </Collapsible>
        </Tile>
        <Tile name="item">
          <ItemGroup className="w-full">
            <Item variant="outline">
              <ItemMedia variant="icon">
                <Star />
              </ItemMedia>
              <ItemContent>
                <ItemTitle>Figma</ItemTitle>
                <p className="text-sm text-muted-foreground">Design tool</p>
              </ItemContent>
            </Item>
            <Item variant="outline">
              <ItemMedia variant="icon">
                <FileText />
              </ItemMedia>
              <ItemContent>
                <ItemTitle>Notion</ItemTitle>
                <p className="text-sm text-muted-foreground">Docs &amp; notes</p>
              </ItemContent>
            </Item>
          </ItemGroup>
        </Tile>
        <Tile name="separator">
          <div className="flex w-full flex-col gap-3">
            <p className="text-sm">Content above</p>
            <Separator />
            <p className="text-sm">Content below</p>
          </div>
        </Tile>
        <Tile name="table">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Name</TableHead>
                <TableHead>Status</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              <TableRow>
                <TableCell>Build</TableCell>
                <TableCell>Passing</TableCell>
              </TableRow>
              <TableRow>
                <TableCell>Deploy</TableCell>
                <TableCell>Pending</TableCell>
              </TableRow>
            </TableBody>
          </Table>
        </Tile>
        <Tile name="tabs">
          <Tabs defaultValue="account" className="w-full">
            <TabsList>
              <TabsTrigger value="account">Account</TabsTrigger>
              <TabsTrigger value="password">Password</TabsTrigger>
            </TabsList>
            <TabsContent value="account" className="text-sm">Account settings here.</TabsContent>
            <TabsContent value="password" className="text-sm">Change password here.</TabsContent>
          </Tabs>
        </Tile>
      </Section>

      <Section title="Layout">
        <Tile name="attachment">
          <Attachment>
            <AttachmentMedia variant="icon">
              <FileText />
            </AttachmentMedia>
            <AttachmentContent>
              <AttachmentTitle>report.pdf</AttachmentTitle>
              <AttachmentDescription>2.4 MB</AttachmentDescription>
            </AttachmentContent>
          </Attachment>
        </Tile>
        <Tile name="direction">
          <DirectionProvider direction="ltr">
            <div className="flex flex-col gap-2">
              <DirText />
              <Button variant="outline" size="sm">RTL-ready button</Button>
            </div>
          </DirectionProvider>
        </Tile>
        <Tile name="resizable">
          <ResizablePanelGroup direction="horizontal" className="h-28 w-full rounded-md border">
            <ResizablePanel defaultSize={50} className="flex items-center justify-center text-sm text-muted-foreground">
              A
            </ResizablePanel>
            <ResizableHandle />
            <ResizablePanel defaultSize={50} className="flex items-center justify-center text-sm text-muted-foreground">
              B
            </ResizablePanel>
          </ResizablePanelGroup>
        </Tile>
        <Tile name="scroll-area">
          <ScrollArea className="h-24 w-full rounded-md border p-3">
            <div className="flex flex-col gap-1.5">
              {Array.from({ length: 12 }, (_, i) => (
                <p key={i} className="text-sm text-muted-foreground">Scroll row {i + 1}</p>
              ))}
            </div>
          </ScrollArea>
        </Tile>
        <Tile name="sidebar">
          <SidebarProvider defaultOpen>
            <Sidebar className="!static !h-44 !w-52 rounded-md border">
              <SidebarHeader>
                <SidebarMenuButton asChild>
                  <a href="#">
                    <Home />
                    <span>Dashboard</span>
                  </a>
                </SidebarMenuButton>
              </SidebarHeader>
              <SidebarContent>
                <SidebarGroup>
                  <SidebarGroupLabel>Menu</SidebarGroupLabel>
                  <SidebarGroupContent>
                    <SidebarMenu>
                      {NAV_LINKS.map(({ label, icon: Icon }) => (
                        <SidebarMenuItem key={label}>
                          <SidebarMenuButton asChild>
                            <a href="#">
                              <Icon />
                              <span>{label}</span>
                            </a>
                          </SidebarMenuButton>
                        </SidebarMenuItem>
                      ))}
                    </SidebarMenu>
                  </SidebarGroupContent>
                </SidebarGroup>
              </SidebarContent>
            </Sidebar>
          </SidebarProvider>
        </Tile>
        <Tile name="form">
          <Form {...form}>
            <form onSubmit={form.handleSubmit(() => toast('Submitted!'))} className="flex w-full flex-col gap-3">
              <FormField
                control={form.control}
                name="name"
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>Name</FormLabel>
                    <FormControl>
                      <Input placeholder="Jane Doe" {...field} />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <Button type="submit" size="sm">Submit</Button>
            </form>
          </Form>
        </Tile>
      </Section>
    </>
  )
}

export default ShadcnGallery
