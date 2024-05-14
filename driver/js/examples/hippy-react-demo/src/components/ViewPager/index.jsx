import React from 'react';
import { StyleSheet, View, Text, ViewPager } from '@hippy/react';

const DEFAULT_DOT_RADIUS = 6;

const styles = StyleSheet.create({
  dotContainer: {
    position: 'absolute',
    bottom: 10,
    left: 0,
    right: 0,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
  },
  dot: {
    width: DEFAULT_DOT_RADIUS,
    height: DEFAULT_DOT_RADIUS,
    // eslint-disable-next-line no-bitwise
    borderRadius: DEFAULT_DOT_RADIUS >> 1,
    // eslint-disable-next-line no-bitwise
    margin: DEFAULT_DOT_RADIUS >> 1,
    backgroundColor: '#BBBBBB',
    // bottom: 16
  },
  selectDot: {
    backgroundColor: '#000000',
  },
  container: {
    height: 500,
  },
  buttonContainer: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    padding: 12,
  },
  button: {
    width: 120,
    height: 36,
    backgroundColor: '#4c9afa',
    borderRadius: 18,
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {
    fontSize: 16,
    color: '#fff',
  },
});

export default class Test extends React.Component {
  middleList = Array(5)
    .fill(0)
    .map((item, index) => (
      <View key={index}>
        <Text> middle索引{index}</Text>
      </View>
    ));

  state = {
    selectedIndex: 0,
    beforeList: [],
    afterList: [],
  };

  constructor(props) {
    super(props);
    this.onPageSelected = this.onPageSelected.bind(this);
    this.onPageScrollStateChanged = this.onPageScrollStateChanged.bind(this);
    this.updateBeforeList = this.updateBeforeList.bind(this);
    this.updateAfterList = this.updateAfterList.bind(this);
  }

  componentDidUpdate() {
    // this.viewpager.setPage(this.state.selectedIndex);
  }

  onPageSelected(pageData) {
    console.log('viewpager:onPageSelected', pageData.position);
    this.setState({
      selectedIndex: pageData.position,
    });
  }

  onPageScrollStateChanged(pageScrollState) {
    console.log('viewpager:onPageScrollStateChanged', pageScrollState);
  }

  onPageScroll({ offset, position }) {
    console.log('viewpager-onPageScroll', offset, position);
  }

  updateBeforeList() {
    const { beforeList, selectedIndex } = this.state;
    const startIndex = -(beforeList.length + 5);
    const moreBeforeList = Array(5)
      .fill(0)
      .map((item, index) => (
        <View key={startIndex + index}>
          <Text> before索引:{startIndex + index}</Text>
        </View>
      ));

    this.setState({
      beforeList: [...moreBeforeList, ...beforeList],
      selectedIndex: selectedIndex + moreBeforeList.length,
    });
  }

  updateAfterList() {
    const { afterList } = this.state;
    const startIndex = afterList.length + 5;
    const moreAfterList = Array(5)
      .fill(0)
      .map((item, index) => (
        <View key={startIndex + index}>
          <Text> after索引:{startIndex + index}</Text>
        </View>
      ));

    this.setState({
      afterList: [...moreAfterList, ...afterList],
    });
  }

  render() {
    const { selectedIndex, beforeList, afterList } = this.state;

    const newChildList = [...beforeList, ...this.middleList, ...afterList];

    return (
      <View style={{ flex: 1, backgroundColor: '#ffffff', marginTop: 50 }}>
        <View style={styles.buttonContainer}>
          <View style={styles.button} onClick={this.updateBeforeList}>
            <Text style={styles.buttonText}>前面新增5个</Text>
          </View>
          <View style={styles.button} onClick={this.updateAfterList}>
            <Text style={styles.buttonText}>后面新增5个</Text>
          </View>
          <View style={styles.button}>
            <Text style={styles.buttonText}>当前索引 {selectedIndex}</Text>
          </View>
        </View>
        <ViewPager
          ref={(ref) => {
            this.viewpager = ref;
          }}
          style={styles.container}
          {/* 可以为下面，但是更新后没有触发onPageSelected */}
          {/* initialPage={selectedIndex}  */}
          initialPage={0}
          keyboardDismissMode="none"
          scrollEnabled
          onPageSelected={this.onPageSelected}
          onPageScrollStateChanged={this.onPageScrollStateChanged}
          onPageScroll={this.onPageScroll}
        >
          {newChildList}
        </ViewPager>
        <View style={styles.dotContainer}>
          {new Array(newChildList.length).fill(0)
            .map((n, i) => {
              const isSelect = i === selectedIndex;
              return (
              // eslint-disable-next-line react/jsx-key
              <View style={[styles.dot, isSelect ? styles.selectDot : null]} key={`dot_${i}`} />
              );
            })}
        </View>
      </View>
    );
  }
}
