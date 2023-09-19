#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  // TDBAL: transmit descriptor base address （Low）
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // 首先，通过读取 E1000_TDT 控制寄存器来询问 E1000 期望接收下一个数据包的 TX 环索引。
  // printf("this is 001!\n");
  acquire(&e1000_lock);
  // regs[E1000_IMS] = 0;
  // __sync_synchronize();

  uint32 tdt = regs[E1000_TDT];
  // 然后检查环是否已溢出。如果在由 E1000_TDT 索引的描述符中 E1000_TXD_STAT_DD 未设置，表示 E1000 尚未完成先前的传输请求，因此返回一个错误。
  // printf("this is 002!\n");
  if(tdt < 0 || tdt >= TX_RING_SIZE){
    printf("tdt not valid! ");
    release(&e1000_lock);
    return -1;
  }
  if (!(tx_ring[tdt].status & E1000_TXD_STAT_DD)) {
    release(&e1000_lock);
    return -1;
  }

  // 否则，使用 mbuffree() 释放上一个从该描述符传输的 mbuf（如果有的话）。
  // printf("this is 003!\n");
  if (tx_mbufs[tdt]) {
    // printf("this is %p!\n", tx_mbufs[tdt]);
    mbuffree(tx_mbufs[tdt]);
  }
  
  // 然后填充描述符。m->head 指向内存中数据包的内容，m->len 是数据包的长度。设置必要的命令标志（查看 E1000 手册的第 3.3 节）并存储 mbuf 的指针以便稍后释放。
  tx_mbufs[tdt] = m;

  tx_ring[tdt].addr = (uint64)tx_mbufs[tdt]->head;
  tx_ring[tdt].length = (uint16)m->len;
  tx_ring[tdt].status = 0;
  tx_ring[tdt].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  // 最后，通过将 E1000_TDT 加 1（对 TX_RING_SIZE 取模）来更新环的位置。
  // printf("this is 005!\n");
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;

  // regs[E1000_IMS] = (1 << 7);
  release(&e1000_lock);

  // printf("this is tx!");
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  while(1){
    // 获取RDT寄存器的值，也就是下一个等待接收的数据包的环索引
    uint32 rdt = regs[E1000_RDT];
    
    // 计算下一个索引（模以环大小）
    uint32 next_rdt = (rdt + 1) % RX_RING_SIZE;
    
    // 检查是否有新的数据包可用
    if (!(rx_ring[next_rdt].status & E1000_RXD_STAT_DD)) {
      // release(&e1000_lock);
      return;
    }
    
    // 更新mbuf的长度
    struct mbuf *m = rx_mbufs[next_rdt];
    m->len = rx_ring[next_rdt].length;
    
    // 将mbuf传递给网络堆栈
    net_rx(m);
    
    // 分配一个新的mbuf以替换旧的
    struct mbuf *new_m = mbufalloc(0);
    if (!new_m) {
      // 处理分配失败
      // release(&e1000_lock);
      return;
    }
    
    // 将新的mbuf数据指针编程到描述符中，并清除状态位
    //rx_mbufs[next_rdt] = new_m;
    rx_ring[next_rdt].addr = (uint64)new_m->head;
    rx_ring[next_rdt].status = 0;
    
    // 保存新的mbuf指针
    rx_mbufs[next_rdt] = new_m;
    
    // 更新RDT寄存器
    regs[E1000_RDT] = next_rdt;
  }

  // printf("this is rx!");
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
