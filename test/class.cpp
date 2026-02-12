class Base {
 public:
  int x;
};

class Derived : public Base {
 public:
  int y;
};

extern "C" {
int base(Base *b) { return b->x; }
int derived(Derived *d) { return d->x + d->y; }
int array(Base bs[32]) { return bs[0].x; }
}

int main() { return 0; }
