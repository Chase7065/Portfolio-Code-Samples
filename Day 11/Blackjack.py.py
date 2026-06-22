import art
from art import CARD
from art import HIDDEN_CARD
import random
import time
<<<<<<< Updated upstream
=======
import subprocess
>>>>>>> Stashed changes

class person:
    def __init__(self, name = "", score = 0, hand = [], spacing = 17):
        self.name = name
        self.score = score
        self.hand = hand
        self.spacing = spacing

dealer = person(name = "Dealer", spacing = 17)
player = person(name = "Player", spacing = ((dealer.spacing * 2) + 1))
BUST = 21
CARD_SPACING = 4
NUM_OF_PLAYERS = 2
UPDATE_SPEED = 0.25
start_of_game = True

dealer.hand = [{'suit_name': "deck",'visible': False}]  # Initialize dealer_row with the hidden card

ranks = {'2': 2, '3': 3, '4': 4, '5': 5, '6': 6, '7': 7, '8': 8, '9': 9, '10': 10, 'J': 10, 'Q': 10, 'K': 10, 'A': 11}
suits = {
    'Spades':   '♠',
    'Diamonds': '♦',
    'Hearts':   '♥',
    'Clubs':    '♣',
}

# Makes and shuffles deck
def make_deck(suits, ranks):
  """Takes ranks and suits, appends a list of dictionaries called 'deck', then shuffles it."""
  deck = []
  for suit in suits:
    for rank in ranks:
      card = {'suit_name': suit, 'rank': rank, 'symbol': suits[suit], 'visible': True}
      deck.append(card)
  random.shuffle(deck)
  return deck

# Display card art
def card_art(card):
  if card['visible'] == False:
    ascii = HIDDEN_CARD
  elif card['visible'] == True:
    ascii = CARD.format(rank = card["rank"], suit = card["symbol"])
  return ascii

# Display a row of cards horizontally strip by strip
def display_row(person):
  image_buffer = []
  strips = []
  first_card = True
  row = person.hand
  spacing = person.spacing

  for card in row:
    strips = card_art(card).splitlines() # Turns vertical ascii into array of horizontal strips {card into horz. strips}

    if first_card == True:
        if person.name == "Dealer":
            for i in range(len(strips)):
                strips[i] = strips[i] + (" " * spacing)
        elif person.name == "Player":
            for i in range(len(strips)):
                strips[i] = (" " * (spacing)) + strips[i]

    first_card = False

    image_buffer.append(strips)

  for strip in zip(*image_buffer):
    print((' ' * CARD_SPACING).join(strip))

# Clears the terminal, Updates dealers and players row of the screen
def update_display():
  global start_of_game
  global player
  global dealer

  print("\033[1;1H", end = "")
  print(art.logo)

  if start_of_game == True:
    time.sleep(UPDATE_SPEED)
  start_of_game = False

  print(" <- DECK -> \t\t\t\t      < Dealer >", end = "")
  time.sleep(UPDATE_SPEED)
  display_row(dealer)

  print("\n\n\t\t\t\t\t      < Player >", end = "")
  display_row(player)
  time.sleep(UPDATE_SPEED)

def deal_cards(person, num_of_cards=2):
  global start_of_game
  global player
  global dealer

  # Deck art / always a hidden card on screen
  if start_of_game == True:
    for card in range(num_of_cards):
      if start_of_game == True:
        update_display()
        deck[-2]['visible'] = False # Dealers first card is covered
      start_of_game = False

      player.hand.append(deck.pop())
      player.score = calculate_score(player.hand)
      update_display()
      dealer.hand.append(deck.pop())
      player.score = calculate_score(player.hand)
      update_display()
    return
  
  else:
    for card in range(num_of_cards):
      person.hand.append(deck.pop())
      player.score = calculate_score(player.hand)
      update_display()


def calculate_score(hand):
  score = 0
  for card in hand:
    if card['visible'] == True:
      score += ranks[card['rank']]
  return score


<<<<<<< Updated upstream
# A list of dictionaries 'cards'
=======
      dealer.hand[_]['visible'] = True
      update_display()
      time.sleep( UPDATE_SPEED )

  player.score = calculate_score(player.hand)
  dealer.score = calculate_score(dealer.hand)
  
  update_display()


# Clear terminal
>>>>>>> Stashed changes
print(f'\033c')
deck = make_deck(suits, ranks)
# print(deck)

# print("Card: ",deck[0], "\n")

starting_cards = 2
deal_cards(starting_cards)

user_choice = ""
while(user_choice is not "h" or player.score <= BUST):
  user_choice = input("\n\n\t\t\t\t       < Hit >   or   < Stand >\n\nChoose, input {Hit <H> or Stand <S>}:  \b").lower()
  if user_choice == "h":
    deal_cards("player", 1)
    if player.score > BUST: break
    

input("Enter to clear screen: ")
print(f'\033c')

# note each card is 14 chars long, half a card is 7 chars
