#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <util/delay_basic.h>
#include "./rfm69.c"
#include "./flash.c"

#define CONFIG_UART
#ifdef CONFIG_UART
#include "./uart.c"
#endif /* CONFIG_UART */


/* selection rotary switch */

#define SEL_DDR DDRC
#define SEL_SHIFT 3
#define SEL_MASK (1 << SEL_SHIFT)

static void sel_setup(void)
{
  /* input pin */
  SEL_DDR &= ~SEL_MASK;

  /* setup adc in free running mode, use aref */
  ADCSRA = 7;
  ADCSRB = 0;
  ADMUX = SEL_SHIFT;
  DIDR0 |= SEL_MASK;
}

static inline void sel_wait_adc(void)
{
  /* 13 cycles per conversion */

  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
  __asm__ __volatile__ ("nop\n");
}

static inline void sel_add_adc(uint16_t* x)
{
  ADCSRA |= 1 << 6;
  while (ADCSRA & (1 << 6)) ;
  *x += ADC & ((1 << 10) - 1);
}

static uint8_t sel_read(void)
{
  /* return the button position */
  /* refer to util/rotary_switch for more info */

  static const uint32_t adc_hi = 1 << 10;
  static const uint32_t npos = 7;

  uint16_t sum;
  uint8_t x;

  /* enable adc and start conversion */
  ADCSRA |= 1 << 7;

  /* wait at least 12 cycles before first conversion */
  sel_wait_adc();

  sum = 0;
  sel_add_adc(&sum);
  sel_add_adc(&sum);
  sel_add_adc(&sum);
  sel_add_adc(&sum);

  /* disable adc */
  ADCSRA &= ~(1 << 7);

  /* FIXME: i put a 12K instead of 120K */
  if (sum <= (0x60 * 4)) sum = 0x08 * 4;

  /* 4 comes from averaging */
  /* 10 is for scaling then rounding down */
  x = (npos * sum * 10) / (adc_hi * 4);
  if ((x % 10) >= 5) return (uint8_t)(1 + x / 10);
  return (uint8_t)(x / 10);
}

#ifdef CONFIG_UART
__attribute__((unused)) static void sel_test(void)
{
  while (1)
  {
    const uint8_t x = sel_read();
    uart_write(uint8_to_string(x), 2);
    uart_write_rn();
    _delay_ms(250);
    _delay_ms(250);
    _delay_ms(250);
    _delay_ms(250);
  }
}
#endif /* CONFIG_UART */


/* slicer global context */

#define PULSE_MAX_COUNT 1024

/* frame pulse duration, in timer units */
static uint8_t pulse_timer[PULSE_MAX_COUNT];

/* current frame pulse count and index */
static volatile uint16_t pulse_count;
static volatile uint16_t pulse_index;

/* end of frame timer */
static uint8_t pulse_eof;

#define PULSE_FLAG_DONE (1 << 0)
#define PULSE_FLAG_OVF (1 << 1)
static volatile uint8_t pulse_flags;

/* pulse timer resolution is 4 or 16 us. this is actually defined */
/* by the timer prescaler value and impacts the maximum timer value */
/* that can be stored in 8 bits (PULSE_MAX_TIMER). we want to stay */
/* on 8 bits to have a maximum buffer size. */
#define PULSE_TIMER_RES_US 16

#define pulse_us_to_timer(__us) (1 + (__us) / PULSE_TIMER_RES_US)
#define pulse_timer_to_us(__x) (((uint16_t)__x) * PULSE_TIMER_RES_US)

/* max is 1024 us with 8 bits counter and 4 us resolution */
/* max is 4096 us with 8 bits counter and 16 us resolution */
#define PULSE_MAX_TIMER 0xff


/* interrupt routines */

static inline void pulse_common_vect(uint8_t flags)
{
  TCCR1B = 0;
  pulse_flags |= flags;
}

/* #define CONFIG_PCINT_ISR */
#ifdef CONFIG_PCINT_ISR
ISR(PCINT2_vect)
#else
static void pcint2_vect(void)
#endif /* CONFIG_PCINT_ISR */
{
  /* capture counter */
  uint16_t n = TCNT1;

  if (pulse_count == PULSE_MAX_COUNT)
  {
    pulse_common_vect(PULSE_FLAG_OVF | PULSE_FLAG_DONE);
    return ;
  }

  /* restart the timer, ctc mode. */
  TCNT1 = 0;
#if (PULSE_TIMER_RES_US == 4)
  TCCR1B = (1 << 3) | (3 << 0);
#elif (PULSE_TIMER_RES_US == 16)
  TCCR1B = (1 << 3) | (4 << 0);
#endif

  /* store counter */
  if (n > PULSE_MAX_TIMER) n = PULSE_MAX_TIMER;
  pulse_timer[pulse_count++] = (uint8_t)n;
}

ISR(TIMER1_OVF_vect)
{
  pulse_common_vect(PULSE_FLAG_DONE);
}

ISR(TIMER1_COMPA_vect)
{
  pulse_common_vect(PULSE_FLAG_DONE);
}

ISR(TIMER1_COMPB_vect)
{
  if (pulse_index == pulse_count)
  {
    pulse_common_vect(PULSE_FLAG_DONE);
    return ;
  }

  /* play pulse_index and increment */
  if ((pulse_index & 1)) rfm69_set_data_high();
  else rfm69_set_data_low();
  TCNT1 = 0;
  OCR1B = pulse_timer[pulse_index++];
}


/* listen for a frame */

static inline uint8_t filter_data(void)
{
  /* glitch filtering: consider a one only if no */
  /* zero appears within a given pulse count */

  uint8_t i;
  uint8_t x = rfm69_get_data();
  for (i = 0; i != 32; ++i) x &= rfm69_get_data();
  return x;
}

static void do_listen(void)
{
  uint8_t pre_state;
  uint8_t cur_state;

  /* prepare the timer. first read in pcint isr. */
  TCCR1B = 0;
  TCNT1 = 0;
  TCCR1A = 0;
  TCCR1C = 0;
  OCR1A = PULSE_MAX_TIMER;
  TIMSK1 = (1 << 1) | (1 << 0);

  /* reset pulse slicer context */
  pulse_count = 0;
  pulse_flags = 0;

  /* put in rx continuous mode */
  rfm69_set_rx_continuous_mode();

  /* setup dio2 so the first bit start the timer */
#ifdef CONFIG_PCINT_ISR
  PCICR |= RFM69_IO_DIO2_PCICR_MASK;
  RFM69_IO_DIO2_PCMSK |= RFM69_IO_DIO2_MASK;
#endif /* CONFIG_PCINT_ISR */

  pre_state = 0;

  /* wait until done */
  while ((pulse_flags & PULSE_FLAG_DONE) == 0)
  {
#ifdef CONFIG_PCINT_ISR

    /* TODO: sleep */

#else

    cur_state = filter_data();
    if (cur_state == pre_state) continue ;
    pre_state = cur_state;
    pcint2_vect();

#endif /* CONFIG_PCINT_ISR */
  }

  /* stop timer */
  TCCR1B = 0;

#ifdef CONFIG_PCINT_ISR
  /* disable dio2 pcint interrupt */
  RFM69_IO_DIO2_PCMSK &= ~RFM69_IO_DIO2_MASK;
  PCICR &= ~RFM69_IO_DIO2_PCICR_MASK;
#endif /* CONFIG_PCINT_ISR */

  /* put back in standby mode */
  rfm69_set_standby_mode();

  /* compute end of frame timer */
  pulse_eof = PULSE_MAX_TIMER;
}


/* store and load frame to and from the flash memory */

/* flash organization */
/* the first subsector is dedicated to global meta data. */
/* starting at subsector 1, there is one subsector per frame. */
/* each frame contains the pulse timer array followed by the */
/* pulse count and end of frame. they are stored using the */
/* same data types as in the code. */

typedef struct
{
#define OOKLONE_META_MAGIC 0xdeadbeef
  uint32_t magic;
  uint8_t version;
  uint8_t frame_count;
} __attribute__((packed)) ooklone_meta_t;

static ooklone_meta_t ooklone_meta;

static void do_store(uint8_t frame_index)
{
  /* store the current frame in flash */

  static const uint16_t meta_page_count = FLASH_SUBSECTOR_PAGE_COUNT;
  static const uint16_t frame_page_count = FLASH_SUBSECTOR_PAGE_COUNT;

  uint8_t n;
  uint16_t i;
  uint8_t* p;
  uint16_t buf[2];

  /* assume there is one subsector for meta data and per frame */
  flash_erase_subsector(1 + frame_index);

  /* store pulse_timer */
  i = meta_page_count + (uint16_t)frame_index * frame_page_count;
  p = pulse_timer;
  n = PULSE_MAX_COUNT / FLASH_PAGE_SIZE;
  for (; n; --n, ++i, p += FLASH_PAGE_SIZE) flash_program_page(i, p);

  /* store pulse_count and pulse_eof */
  buf[0] = pulse_count;
  buf[1] = pulse_eof;
  flash_program_bytes(i, (const uint8_t*)buf, sizeof(buf));

  /* update metadata */
  if (frame_index >= ooklone_meta.frame_count)
  {
    ooklone_meta.frame_count = frame_index + 1;
    flash_erase_subsector(0);
    flash_program_bytes(0, (uint8_t*)&ooklone_meta, sizeof(ooklone_meta));
  }
}

static void do_load(uint8_t frame_index)
{
  /* load the current frame from flash */

  static const uint16_t meta_page_count = FLASH_SUBSECTOR_PAGE_COUNT;
  static const uint16_t frame_page_count = FLASH_SUBSECTOR_PAGE_COUNT;

  /* assume frame_index < ooklone_meta.frame_count */
  uint8_t n;
  uint8_t* p;
  uint16_t i;
  uint16_t buf[2];

  /* load pulse_timer */
  i = meta_page_count + (uint16_t)frame_index * frame_page_count;
  p = pulse_timer;
  n = PULSE_MAX_COUNT / FLASH_PAGE_SIZE;
  for (; n; --n, ++i, p += FLASH_PAGE_SIZE) flash_read_page(i, p);

  /* read pulse_count and pulse_eof */
  flash_read_bytes(i, (uint8_t*)buf, sizeof(buf));
  pulse_count = buf[0];
  pulse_eof = buf[1];
}


/* slicer context printing */

#ifdef CONFIG_UART
static void do_print(void)
{
  uint16_t i;

  UART_WRITE_STRING("flags: ");
  uart_write(uint8_to_string(pulse_flags), 2);
  uart_write_rn();

  UART_WRITE_STRING("eof  : ");
  uart_write(uint8_to_string(pulse_eof), 2);
  uart_write_rn();

  for (i = 0; i != pulse_count; ++i)
  {
    const uint16_t us = pulse_timer_to_us(pulse_timer[i]);

    if ((i & 0x7) == 0)
    {
      uart_write_rn();
      uart_write(uint16_to_string(i), 4);
    }

    UART_WRITE_STRING(" ");
    uart_write(uint16_to_string(us), 4);
  }

  uart_write_rn();
}
#endif /* CONFIG_UART */


/* frame replay */

static void delay_eof_timer(uint8_t x)
{
  /* busy wait for end of frame delay given in timer ticks */

#if (PULSE_TIMER_RES_US == 4)
  static const uint16_t prescal = 64;
#elif (PULSE_TIMER_RES_US == 16)
  static const uint16_t prescal = 256;
#endif

  _delay_loop_2((uint16_t)x * prescal);
}

static void do_replay(void)
{
  /* replay the currently stored pulses */

  /* assume pulse_timer, pulse_count and pulse_eof valid */

  /* prepare context */
  pulse_flags = 0;
  pulse_index = 1;

  /* put in tx continuous mode */
  rfm69_set_tx_continuous_mode();

  rfm69_set_data_low();

  /* restart the timer, ctc mode, 16us resolution. */
  /* ocr1b used for top, no max value */
  /* top value is 0x100 or 4.08 ms. */
  TCCR1A = 0;
  TCNT1 = 0;
  TCCR1C = 0;
  OCR1B = 0xff;
  TIMSK1 = 1 << 2;
#if (PULSE_TIMER_RES_US == 4)
  TCCR1B = 3 << 0;
#elif (PULSE_TIMER_RES_US == 16)
  TCCR1B = 4 << 0;
#endif

  while ((pulse_flags & PULSE_FLAG_DONE) == 0)
  {
    /* TODO: sleep */
  }

  rfm69_set_data_low();

  /* disable counter */
  TCCR1B = 0;

  /* put back in standby mode */
  rfm69_set_standby_mode();

  delay_eof_timer(pulse_eof);
}


/* buttons */

#define BUT_COMMON_DDR DDRC
#define BUT_COMMON_PORT PORTC
#define BUT_COMMON_PIN PINC
#define BUT_PLAY_MASK (1 << 0)
#define BUT_RECORD_MASK (1 << 1)
#define BUT_ALL_MASK (BUT_RECORD_MASK | BUT_PLAY_MASK)
#define BUT_COMMON_PCICR_MASK (1 << 1)
#define BUT_COMMON_PCMSK PCMSK1

static volatile uint8_t but_pcint_pin;

ISR(PCINT1_vect)
{
  /* capture pin values */
  but_pcint_pin = BUT_COMMON_PIN;
}

static void but_setup(void)
{
  /* set as input, enable pullups */
  BUT_COMMON_DDR &= ~BUT_ALL_MASK;
  BUT_COMMON_PORT |= BUT_ALL_MASK;

  but_pcint_pin = BUT_ALL_MASK;
}

static uint8_t but_wait(void)
{
  uint8_t x = BUT_ALL_MASK;

  /* sleep mode */
  set_sleep_mode(SLEEP_MODE_IDLE);

  /* enable pin change interrupt */
  PCICR |= BUT_COMMON_PCICR_MASK;
  BUT_COMMON_PCMSK |= BUT_ALL_MASK;

  while (x == BUT_ALL_MASK)
  {
    /* capture but_pcint_pin with interrupts disabled */
    /* if no pin set, then sleep until pcint */
    /* take care of the inverted logic due to pullups */

    cli();

    if ((but_pcint_pin & BUT_ALL_MASK) == BUT_ALL_MASK)
    {
      sleep_enable();
      sei();
      sleep_cpu();
      sleep_bod_disable();
    }

    x = but_pcint_pin;

    sei();

    /* simple debouncing logic. 1 wins over 0. */
    x |= BUT_COMMON_PIN;
    _delay_us(1);
    x |= BUT_COMMON_PIN;
    _delay_us(1);
    x |= BUT_COMMON_PIN;
    _delay_us(1);
    x |= BUT_COMMON_PIN;

    /* conserve only pins of interest */
    x &= BUT_ALL_MASK;
  }

  /* disable pin change interrupt */
  BUT_COMMON_PCMSK &= ~BUT_ALL_MASK;
  PCICR &= ~BUT_COMMON_PCICR_MASK;

  /* inverted logic because of pullups */
  return x ^ BUT_ALL_MASK;
}


/* main */

int main(void)
{
  uint8_t x;
  uint8_t i;
  uint8_t frame_index;

#ifdef CONFIG_UART
  uart_setup();
#endif /* CONFIG_UART */

  spi_setup_master();
  rfm69_setup();
  flash_setup();
  but_setup();
  sel_setup();

  sei();

  /* read the ooklone meta from flash */
  flash_read_bytes(0, (uint8_t*)&ooklone_meta, sizeof(ooklone_meta));
  if (ooklone_meta.magic != OOKLONE_META_MAGIC)
  {
    ooklone_meta.magic = OOKLONE_META_MAGIC;
    ooklone_meta.version = 0;
    ooklone_meta.frame_count = 0;
  }

  while (1)
  {
    x = but_wait();

    /* read the selection switch */
    frame_index = sel_read();

    if (x & BUT_RECORD_MASK)
    {
#ifdef CONFIG_UART
      UART_WRITE_STRING("record");
      uart_write_rn();
#endif /* CONFIG_UART */

      do_listen();
      do_store(frame_index);

#ifdef CONFIG_UART
      do_print();
#endif /* CONFIG_UART */
    }

    if (x & BUT_PLAY_MASK)
    {
#ifdef CONFIG_UART
      UART_WRITE_STRING("play");
      uart_write_rn();
#endif /* CONFIG_UART */

      do_load(frame_index);

#ifdef CONFIG_UART
      do_print();
#endif /* CONFIG_UART */

      for (i = 0; i != 7; ++i) do_replay();
    }
  }

  return 0;
}
