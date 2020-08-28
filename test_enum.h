/*
enum chaos {
  beg __attribute__((annotate("reflect-property")))  __attribute__((annotate("reflect-property"))),
  end __attribute__((annotate("reflect-property")))
};
*/

#define NameAndLabelAttr(name, labels) \
  __attribute__((annotate("name:" name))) __attribute__((annotate("labels:" labels)))
enum Counter {
  counter1 NameAndLabelAttr("counter1_name", "counter1_label1,counter1_label2"),
  counter2 NameAndLabelAttr("counter2_name", "counter2_label1,counter2_label2"),
};

enum Gauge {
  guage1 NameAndLabelAttr("guage1_name", "guage1_label1,guage1_label2"),
  guage2 NameAndLabelAttr("guage2_name", "guage2_label1,guage2_label2"),
};

enum Histogram {
  hist1 NameAndLabelAttr("hist1_name", "hist1_label1,hist1_label2"),
  hist2 NameAndLabelAttr("hist2_name", "hist2_label1,hist2_label2"),
};

