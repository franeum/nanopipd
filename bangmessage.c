#include "m_pd.h"
#include <wiringPi.h>

static t_class *bangmessage_class;


typedef struct _bangmessage {
  t_object  x_obj;
  int count;
  int led;
} t_bangmessage;


void bangmessage_bang(t_bangmessage *x)
{
  //post("Hello world !!");
  count++;
  if (count % 2 == 1) {
    post("LED ON");
    digitalWrite(x->led, HIGH);
  } else {
    post("LED OFF");
    digitalWrite(x->led, LOW);
  }
}


void *bangmessage_new(void)
{
  t_bangmessage *x = (t_bangmessage *)pd_new(bangmessage_class);
  if (wiringPiSetup() == -1) post("wiringPi not loaded!");
  x->led = 7;
  pinMode(led, OUTPUT);
  count = 0;
  return (void *)x;
}

void bangmessage_setup(void) {
  bangmessage_class = class_new(gensym("bangmessage"),        /* the object's name is "bangmessage" */
			       (t_newmethod)bangmessage_new, /* the object's constructor is "bangmessage_new()" */
			       0,                           /* no special destructor */
			       sizeof(t_bangmessage),        /* the size of the data-space */
			       CLASS_DEFAULT,               /* a normal pd object */
			       0);                          /* no creation arguments */

  class_addbang(bangmessage_class, bangmessage_bang);
}
